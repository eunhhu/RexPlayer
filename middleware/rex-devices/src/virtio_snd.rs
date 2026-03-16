//! Virtio sound device backend
//!
//! Implements the virtio-snd device per virtio specification (Section 5.14).
//! Provides PCM playback and capture with jack/chmap info support.
//! Integrates with host audio API via the AudioBackend trait.

use crate::{DeviceError, DeviceResult, VirtioDevice, VirtioDeviceType};
use std::collections::VecDeque;

// ============================================================================
// Command / response codes (virtio spec 5.14.6)
// ============================================================================

pub mod cmd {
    // Jack commands
    pub const VIRTIO_SND_R_JACK_INFO: u32 = 1;
    pub const VIRTIO_SND_R_JACK_REMAP: u32 = 2;

    // PCM commands
    pub const VIRTIO_SND_R_PCM_INFO: u32 = 0x0100;
    pub const VIRTIO_SND_R_PCM_SET_PARAMS: u32 = 0x0101;
    pub const VIRTIO_SND_R_PCM_PREPARE: u32 = 0x0102;
    pub const VIRTIO_SND_R_PCM_RELEASE: u32 = 0x0103;
    pub const VIRTIO_SND_R_PCM_START: u32 = 0x0104;
    pub const VIRTIO_SND_R_PCM_STOP: u32 = 0x0105;

    // Channel map commands
    pub const VIRTIO_SND_R_CHMAP_INFO: u32 = 0x0200;

    // Response status codes
    pub const VIRTIO_SND_S_OK: u32 = 0x8000;
    pub const VIRTIO_SND_S_BAD_MSG: u32 = 0x8001;
    pub const VIRTIO_SND_S_NOT_SUPP: u32 = 0x8002;
    pub const VIRTIO_SND_S_IO_ERR: u32 = 0x8003;
}

// ============================================================================
// Virtqueue indices
// ============================================================================

/// Control queue: driver → device commands
pub const CONTROL_QUEUE: u16 = 0;
/// Event queue: device → driver notifications
pub const EVENT_QUEUE: u16 = 1;
/// TX queue: driver → device (PCM playback data)
pub const TX_QUEUE: u16 = 2;
/// RX queue: device → driver (PCM capture data)
pub const RX_QUEUE: u16 = 3;

// ============================================================================
// Audio sample formats
// ============================================================================

/// Supported PCM sample formats
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleFormat {
    /// Unsigned 8-bit
    U8 = 0,
    /// Signed 16-bit little-endian
    S16Le = 2,
    /// Signed 32-bit little-endian
    S32Le = 6,
    /// 32-bit float little-endian
    FloatLe = 8,
}

impl SampleFormat {
    /// Parse from u8 value
    pub fn from_u8(val: u8) -> Option<Self> {
        match val {
            0 => Some(SampleFormat::U8),
            2 => Some(SampleFormat::S16Le),
            6 => Some(SampleFormat::S32Le),
            8 => Some(SampleFormat::FloatLe),
            _ => None,
        }
    }

    /// Size of one sample in bytes
    pub fn sample_bytes(&self) -> usize {
        match self {
            SampleFormat::U8 => 1,
            SampleFormat::S16Le => 2,
            SampleFormat::S32Le => 4,
            SampleFormat::FloatLe => 4,
        }
    }

    /// Bitmask for this format in PCM info
    pub fn to_bitmask(&self) -> u64 {
        1u64 << (*self as u8)
    }
}

// ============================================================================
// Audio sample rates
// ============================================================================

/// Supported sample rates
pub mod rates {
    pub const RATE_8000: u32 = 8000;
    pub const RATE_11025: u32 = 11025;
    pub const RATE_16000: u32 = 16000;
    pub const RATE_22050: u32 = 22050;
    pub const RATE_44100: u32 = 44100;
    pub const RATE_48000: u32 = 48000;
    pub const RATE_96000: u32 = 96000;

    /// All supported rates (for iteration)
    pub const ALL: &[u32] = &[
        RATE_8000, RATE_11025, RATE_16000, RATE_22050,
        RATE_44100, RATE_48000, RATE_96000,
    ];

    /// Rate index bits for the supported_rates bitmask
    pub fn rate_bit(rate: u32) -> Option<u64> {
        match rate {
            5512 => Some(1 << 0),
            8000 => Some(1 << 1),
            11025 => Some(1 << 2),
            16000 => Some(1 << 3),
            22050 => Some(1 << 4),
            32000 => Some(1 << 5),
            44100 => Some(1 << 6),
            48000 => Some(1 << 7),
            64000 => Some(1 << 8),
            88200 => Some(1 << 9),
            96000 => Some(1 << 10),
            176400 => Some(1 << 11),
            192000 => Some(1 << 12),
            _ => None,
        }
    }
}

// ============================================================================
// PCM stream direction
// ============================================================================

/// Direction of a PCM stream
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StreamDirection {
    /// Output (playback)
    Playback = 0,
    /// Input (capture)
    Capture = 1,
}

impl StreamDirection {
    pub fn from_u8(val: u8) -> Option<Self> {
        match val {
            0 => Some(StreamDirection::Playback),
            1 => Some(StreamDirection::Capture),
            _ => None,
        }
    }
}

// ============================================================================
// PCM stream state machine
// ============================================================================

/// State of a PCM stream (virtio spec 5.14.6.6.1)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PcmState {
    /// Initial state — no parameters set
    Unset,
    /// Parameters have been set
    SetParams,
    /// Stream has been prepared (buffers allocated)
    Prepared,
    /// Stream is running (data flowing)
    Running,
    /// Stream has been stopped
    Stopped,
    /// Stream has been released
    Released,
}

impl PcmState {
    /// Check if a transition to the given target state is valid
    pub fn can_transition_to(&self, target: PcmState) -> bool {
        matches!(
            (self, target),
            // SET_PARAMS can be called from Unset or SetParams
            (PcmState::Unset, PcmState::SetParams)
            | (PcmState::SetParams, PcmState::SetParams)
            // PREPARE from SetParams or Stopped or Prepared
            | (PcmState::SetParams, PcmState::Prepared)
            | (PcmState::Stopped, PcmState::Prepared)
            | (PcmState::Prepared, PcmState::Prepared)
            // START from Prepared
            | (PcmState::Prepared, PcmState::Running)
            // STOP from Running
            | (PcmState::Running, PcmState::Stopped)
            // RELEASE from Stopped or Prepared or SetParams
            | (PcmState::Stopped, PcmState::Released)
            | (PcmState::Prepared, PcmState::Released)
            | (PcmState::SetParams, PcmState::Released)
        )
    }
}

// ============================================================================
// PCM parameters
// ============================================================================

/// Parameters for a PCM stream
#[derive(Debug, Clone)]
pub struct PcmParams {
    /// Size of the audio buffer in bytes
    pub buffer_bytes: u32,
    /// Size of one period in bytes
    pub period_bytes: u32,
    /// Number of channels
    pub channels: u8,
    /// Sample format
    pub format: SampleFormat,
    /// Sample rate in Hz
    pub rate: u32,
}

impl Default for PcmParams {
    fn default() -> Self {
        Self {
            buffer_bytes: 8192,
            period_bytes: 4096,
            channels: 2,
            format: SampleFormat::S16Le,
            rate: 48000,
        }
    }
}

// ============================================================================
// PCM stream
// ============================================================================

/// A single PCM audio stream
#[derive(Debug)]
pub struct PcmStream {
    /// Stream ID
    pub id: u32,
    /// Stream direction
    pub direction: StreamDirection,
    /// Current state
    pub state: PcmState,
    /// Stream parameters (set via SET_PARAMS)
    pub params: PcmParams,
    /// Audio ring buffer
    pub buffer: VecDeque<u8>,
    /// Total bytes written to this stream (cumulative)
    pub bytes_written: u64,
    /// Total bytes read from this stream (cumulative)
    pub bytes_read: u64,
}

impl PcmStream {
    /// Create a new stream with the given ID and direction
    pub fn new(id: u32, direction: StreamDirection) -> Self {
        Self {
            id,
            direction,
            state: PcmState::Unset,
            params: PcmParams::default(),
            buffer: VecDeque::with_capacity(8192),
            bytes_written: 0,
            bytes_read: 0,
        }
    }

    /// Set parameters for this stream
    pub fn set_params(&mut self, params: PcmParams) -> DeviceResult<()> {
        if !self.state.can_transition_to(PcmState::SetParams) {
            return Err(DeviceError::UnsupportedFeature(format!(
                "cannot SET_PARAMS from state {:?}",
                self.state
            )));
        }
        self.params = params;
        self.state = PcmState::SetParams;
        Ok(())
    }

    /// Prepare the stream (allocate buffers)
    pub fn prepare(&mut self) -> DeviceResult<()> {
        if !self.state.can_transition_to(PcmState::Prepared) {
            return Err(DeviceError::UnsupportedFeature(format!(
                "cannot PREPARE from state {:?}",
                self.state
            )));
        }
        self.buffer = VecDeque::with_capacity(self.params.buffer_bytes as usize);
        self.state = PcmState::Prepared;
        Ok(())
    }

    /// Start the stream
    pub fn start(&mut self) -> DeviceResult<()> {
        if !self.state.can_transition_to(PcmState::Running) {
            return Err(DeviceError::UnsupportedFeature(format!(
                "cannot START from state {:?}",
                self.state
            )));
        }
        self.state = PcmState::Running;
        Ok(())
    }

    /// Stop the stream
    pub fn stop(&mut self) -> DeviceResult<()> {
        if !self.state.can_transition_to(PcmState::Stopped) {
            return Err(DeviceError::UnsupportedFeature(format!(
                "cannot STOP from state {:?}",
                self.state
            )));
        }
        self.state = PcmState::Stopped;
        Ok(())
    }

    /// Release the stream
    pub fn release(&mut self) -> DeviceResult<()> {
        if !self.state.can_transition_to(PcmState::Released) {
            return Err(DeviceError::UnsupportedFeature(format!(
                "cannot RELEASE from state {:?}",
                self.state
            )));
        }
        self.buffer.clear();
        self.state = PcmState::Released;
        Ok(())
    }

    /// Write samples into the stream buffer (playback path)
    pub fn write_samples(&mut self, data: &[u8]) -> DeviceResult<usize> {
        if self.state != PcmState::Running {
            return Err(DeviceError::NotReady);
        }
        let capacity = self.params.buffer_bytes as usize;
        let available = capacity.saturating_sub(self.buffer.len());
        let to_write = data.len().min(available);
        self.buffer.extend(&data[..to_write]);
        self.bytes_written += to_write as u64;
        Ok(to_write)
    }

    /// Read samples from the stream buffer (capture path)
    pub fn read_samples(&mut self, buf: &mut [u8]) -> DeviceResult<usize> {
        if self.state != PcmState::Running {
            return Err(DeviceError::NotReady);
        }
        let n = buf.len().min(self.buffer.len());
        for (i, byte) in self.buffer.drain(..n).enumerate() {
            buf[i] = byte;
        }
        self.bytes_read += n as u64;
        Ok(n)
    }

    /// Get the number of bytes currently in the buffer
    pub fn buffered_bytes(&self) -> usize {
        self.buffer.len()
    }
}

// ============================================================================
// Jack info
// ============================================================================

/// Information about an audio jack
#[derive(Debug, Clone)]
pub struct JackInfo {
    /// Jack ID
    pub id: u32,
    /// HDA register default configuration
    pub hda_reg_defconf: u32,
    /// HDA function group type
    pub hda_fn_nid: u32,
    /// Feature flags
    pub features: u32,
    /// Whether the jack is connected
    pub connected: bool,
}

impl JackInfo {
    /// Create a default output jack
    pub fn default_output(id: u32) -> Self {
        Self {
            id,
            hda_reg_defconf: 0x0121_1010, // Line Out, 1/8 inch jack
            hda_fn_nid: 0,
            features: 0,
            connected: true,
        }
    }

    /// Create a default input jack
    pub fn default_input(id: u32) -> Self {
        Self {
            id,
            hda_reg_defconf: 0x0181_3010, // Microphone, 1/8 inch jack
            hda_fn_nid: 0,
            features: 0,
            connected: true,
        }
    }
}

// ============================================================================
// PCM info (virtio spec 5.14.6.6)
// ============================================================================

/// Information about a PCM stream, returned by VIRTIO_SND_R_PCM_INFO
#[derive(Debug, Clone)]
pub struct PcmInfo {
    /// Stream ID
    pub id: u32,
    /// HDA function group NID
    pub hda_fn_nid: u32,
    /// Feature flags
    pub features: u32,
    /// Bitmask of supported formats
    pub formats: u64,
    /// Bitmask of supported rates
    pub supported_rates: u64,
    /// Stream direction
    pub direction: StreamDirection,
    /// Minimum number of channels
    pub channels_min: u8,
    /// Maximum number of channels
    pub channels_max: u8,
}

impl PcmInfo {
    /// Create default PCM info for a playback stream
    pub fn default_playback(id: u32) -> Self {
        let formats = SampleFormat::U8.to_bitmask()
            | SampleFormat::S16Le.to_bitmask()
            | SampleFormat::S32Le.to_bitmask()
            | SampleFormat::FloatLe.to_bitmask();

        let mut supported_rates = 0u64;
        for &rate in rates::ALL {
            if let Some(bit) = rates::rate_bit(rate) {
                supported_rates |= bit;
            }
        }

        Self {
            id,
            hda_fn_nid: 0,
            features: 0,
            formats,
            supported_rates,
            direction: StreamDirection::Playback,
            channels_min: 1,
            channels_max: 8,
        }
    }

    /// Create default PCM info for a capture stream
    pub fn default_capture(id: u32) -> Self {
        let mut info = Self::default_playback(id);
        info.direction = StreamDirection::Capture;
        info.channels_max = 2;
        info
    }
}

// ============================================================================
// Channel map info
// ============================================================================

/// Standard channel positions
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChannelPosition {
    FrontLeft = 0,
    FrontRight = 1,
    FrontCenter = 2,
    Lfe = 3,
    RearLeft = 4,
    RearRight = 5,
    SideLeft = 9,
    SideRight = 10,
}

/// Channel map information
#[derive(Debug, Clone)]
pub struct ChannelMapInfo {
    /// Channel map ID
    pub id: u32,
    /// Direction (playback or capture)
    pub direction: StreamDirection,
    /// Number of channels
    pub channels: u8,
    /// Channel positions
    pub positions: Vec<ChannelPosition>,
}

impl ChannelMapInfo {
    /// Create a stereo channel map
    pub fn stereo(id: u32, direction: StreamDirection) -> Self {
        Self {
            id,
            direction,
            channels: 2,
            positions: vec![ChannelPosition::FrontLeft, ChannelPosition::FrontRight],
        }
    }

    /// Create a 5.1 surround channel map
    pub fn surround_5_1(id: u32, direction: StreamDirection) -> Self {
        Self {
            id,
            direction,
            channels: 6,
            positions: vec![
                ChannelPosition::FrontLeft,
                ChannelPosition::FrontRight,
                ChannelPosition::FrontCenter,
                ChannelPosition::Lfe,
                ChannelPosition::RearLeft,
                ChannelPosition::RearRight,
            ],
        }
    }
}

// ============================================================================
// Command header (virtio spec 5.14.6.1)
// ============================================================================

/// Common header for all sound commands
#[derive(Debug, Clone, Copy)]
pub struct SndHdr {
    /// Command code (VIRTIO_SND_R_*)
    pub code: u32,
}

impl SndHdr {
    pub fn from_bytes(buf: &[u8]) -> DeviceResult<Self> {
        if buf.len() < 4 {
            return Err(DeviceError::InvalidDescriptor);
        }
        Ok(Self {
            code: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
        })
    }

    pub fn to_bytes(&self) -> [u8; 4] {
        self.code.to_le_bytes()
    }
}

// ============================================================================
// Audio backend trait
// ============================================================================

/// Trait for audio output/input backends.
///
/// Implementations handle the actual audio I/O with the host system
/// (e.g., PulseAudio, PipeWire, CoreAudio, ALSA).
pub trait AudioBackend: Send {
    /// Write PCM samples for playback.
    /// Returns the number of bytes consumed.
    fn write_samples(&mut self, stream_id: u32, data: &[u8]) -> DeviceResult<usize>;

    /// Read PCM samples from capture.
    /// Returns the number of bytes read.
    fn read_samples(&mut self, stream_id: u32, buf: &mut [u8]) -> DeviceResult<usize>;

    /// Start audio processing for a stream.
    fn start(&mut self, stream_id: u32) -> DeviceResult<()>;

    /// Stop audio processing for a stream.
    fn stop(&mut self, stream_id: u32) -> DeviceResult<()>;
}

// ============================================================================
// NullBackend (drops audio, useful for testing)
// ============================================================================

/// A null audio backend that silently drops all audio data.
///
/// Useful for testing and headless operation where audio output is not needed.
pub struct NullBackend {
    /// Total bytes "written" (dropped)
    pub bytes_dropped: u64,
    /// Total bytes "read" (generated as silence)
    pub bytes_generated: u64,
}

impl NullBackend {
    pub fn new() -> Self {
        Self {
            bytes_dropped: 0,
            bytes_generated: 0,
        }
    }
}

impl Default for NullBackend {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioBackend for NullBackend {
    fn write_samples(&mut self, _stream_id: u32, data: &[u8]) -> DeviceResult<usize> {
        self.bytes_dropped += data.len() as u64;
        Ok(data.len())
    }

    fn read_samples(&mut self, _stream_id: u32, buf: &mut [u8]) -> DeviceResult<usize> {
        // Generate silence
        for b in buf.iter_mut() {
            *b = 0;
        }
        self.bytes_generated += buf.len() as u64;
        Ok(buf.len())
    }

    fn start(&mut self, _stream_id: u32) -> DeviceResult<()> {
        Ok(())
    }

    fn stop(&mut self, _stream_id: u32) -> DeviceResult<()> {
        Ok(())
    }
}

// ============================================================================
// Sound device configuration
// ============================================================================

/// Configuration for the virtio-snd device
#[derive(Debug, Clone)]
pub struct SndConfig {
    /// Number of jacks
    pub num_jacks: u32,
    /// Number of PCM streams
    pub num_streams: u32,
    /// Number of channel maps
    pub num_chmaps: u32,
}

impl Default for SndConfig {
    fn default() -> Self {
        Self {
            num_jacks: 2,     // output + input
            num_streams: 2,   // playback + capture
            num_chmaps: 2,    // stereo playback + stereo capture
        }
    }
}

/// Size of the sound device config space in bytes (3 x u32 = 12)
pub const CONFIG_SPACE_SIZE: usize = 12;

// ============================================================================
// VirtioSnd device
// ============================================================================

/// Virtio sound device.
///
/// Provides virtual audio jacks, PCM streams, and channel maps to the guest.
/// Audio data flows through the TX/RX virtqueues and is processed by the
/// configured AudioBackend.
pub struct VirtioSnd {
    /// Device configuration
    config: SndConfig,
    /// Feature flags
    features: u64,
    /// Whether the device has been activated
    activated: bool,
    /// Audio jacks
    jacks: Vec<JackInfo>,
    /// PCM stream info descriptors
    pcm_infos: Vec<PcmInfo>,
    /// PCM stream instances (state machines)
    streams: Vec<PcmStream>,
    /// Channel map descriptors
    chmaps: Vec<ChannelMapInfo>,
    /// Audio backend
    backend: Box<dyn AudioBackend>,
}

impl VirtioSnd {
    /// Create a new sound device with default configuration and NullBackend.
    pub fn new() -> Self {
        Self::with_config_and_backend(SndConfig::default(), Box::new(NullBackend::new()))
    }

    /// Create a new sound device with a specific configuration and backend.
    pub fn with_config_and_backend(config: SndConfig, backend: Box<dyn AudioBackend>) -> Self {
        // Create default jacks
        let mut jacks = Vec::new();
        if config.num_jacks >= 1 {
            jacks.push(JackInfo::default_output(0));
        }
        if config.num_jacks >= 2 {
            jacks.push(JackInfo::default_input(1));
        }

        // Create default PCM streams
        let mut pcm_infos = Vec::new();
        let mut streams = Vec::new();
        if config.num_streams >= 1 {
            pcm_infos.push(PcmInfo::default_playback(0));
            streams.push(PcmStream::new(0, StreamDirection::Playback));
        }
        if config.num_streams >= 2 {
            pcm_infos.push(PcmInfo::default_capture(1));
            streams.push(PcmStream::new(1, StreamDirection::Capture));
        }

        // Create default channel maps
        let mut chmaps = Vec::new();
        if config.num_chmaps >= 1 {
            chmaps.push(ChannelMapInfo::stereo(0, StreamDirection::Playback));
        }
        if config.num_chmaps >= 2 {
            chmaps.push(ChannelMapInfo::stereo(1, StreamDirection::Capture));
        }

        Self {
            config,
            features: 0,
            activated: false,
            jacks,
            pcm_infos,
            streams,
            chmaps,
            backend,
        }
    }

    // ========================================================================
    // Config space
    // ========================================================================

    /// Read the config space (12 bytes, little-endian).
    ///
    /// Layout:
    /// - offset 0: jacks (u32)
    /// - offset 4: streams (u32)
    /// - offset 8: chmaps (u32)
    pub fn read_config(&self) -> [u8; CONFIG_SPACE_SIZE] {
        let mut buf = [0u8; CONFIG_SPACE_SIZE];
        buf[0..4].copy_from_slice(&self.config.num_jacks.to_le_bytes());
        buf[4..8].copy_from_slice(&self.config.num_streams.to_le_bytes());
        buf[8..12].copy_from_slice(&self.config.num_chmaps.to_le_bytes());
        buf
    }

    // ========================================================================
    // Command processing
    // ========================================================================

    /// Process a control queue command and return a response status.
    pub fn process_command(&mut self, cmd_data: &[u8]) -> u32 {
        if cmd_data.len() < 4 {
            return cmd::VIRTIO_SND_S_BAD_MSG;
        }

        let hdr = match SndHdr::from_bytes(cmd_data) {
            Ok(h) => h,
            Err(_) => return cmd::VIRTIO_SND_S_BAD_MSG,
        };

        match hdr.code {
            cmd::VIRTIO_SND_R_JACK_INFO => cmd::VIRTIO_SND_S_OK,
            cmd::VIRTIO_SND_R_JACK_REMAP => cmd::VIRTIO_SND_S_NOT_SUPP,
            cmd::VIRTIO_SND_R_PCM_INFO => cmd::VIRTIO_SND_S_OK,
            cmd::VIRTIO_SND_R_PCM_SET_PARAMS => self.handle_pcm_set_params(cmd_data),
            cmd::VIRTIO_SND_R_PCM_PREPARE => self.handle_pcm_transition(cmd_data, PcmState::Prepared),
            cmd::VIRTIO_SND_R_PCM_START => self.handle_pcm_transition(cmd_data, PcmState::Running),
            cmd::VIRTIO_SND_R_PCM_STOP => self.handle_pcm_transition(cmd_data, PcmState::Stopped),
            cmd::VIRTIO_SND_R_PCM_RELEASE => self.handle_pcm_transition(cmd_data, PcmState::Released),
            cmd::VIRTIO_SND_R_CHMAP_INFO => cmd::VIRTIO_SND_S_OK,
            _ => cmd::VIRTIO_SND_S_NOT_SUPP,
        }
    }

    /// Handle PCM SET_PARAMS command.
    ///
    /// Expected payload after the 4-byte header:
    /// - stream_id: u32 (offset 4)
    /// - buffer_bytes: u32 (offset 8)
    /// - period_bytes: u32 (offset 12)
    /// - channels: u8 (offset 16)
    /// - format: u8 (offset 17)
    /// - rate: u8 (offset 18)
    fn handle_pcm_set_params(&mut self, data: &[u8]) -> u32 {
        if data.len() < 24 {
            return cmd::VIRTIO_SND_S_BAD_MSG;
        }

        let stream_id = u32::from_le_bytes([data[4], data[5], data[6], data[7]]);
        let buffer_bytes = u32::from_le_bytes([data[8], data[9], data[10], data[11]]);
        let period_bytes = u32::from_le_bytes([data[12], data[13], data[14], data[15]]);
        let channels = data[16];
        let format_raw = data[17];
        let rate_raw = data[18];

        let format = match SampleFormat::from_u8(format_raw) {
            Some(f) => f,
            None => return cmd::VIRTIO_SND_S_NOT_SUPP,
        };

        // Map rate index to actual rate
        let rate = match Self::rate_index_to_hz(rate_raw) {
            Some(r) => r,
            None => return cmd::VIRTIO_SND_S_NOT_SUPP,
        };

        let params = PcmParams {
            buffer_bytes,
            period_bytes,
            channels,
            format,
            rate,
        };

        if let Some(stream) = self.streams.get_mut(stream_id as usize) {
            match stream.set_params(params) {
                Ok(()) => cmd::VIRTIO_SND_S_OK,
                Err(_) => cmd::VIRTIO_SND_S_IO_ERR,
            }
        } else {
            cmd::VIRTIO_SND_S_BAD_MSG
        }
    }

    /// Handle PCM state transition commands (PREPARE/START/STOP/RELEASE).
    ///
    /// Payload after header: stream_id: u32 (offset 4)
    fn handle_pcm_transition(&mut self, data: &[u8], target: PcmState) -> u32 {
        if data.len() < 8 {
            return cmd::VIRTIO_SND_S_BAD_MSG;
        }

        let stream_id = u32::from_le_bytes([data[4], data[5], data[6], data[7]]);

        if let Some(stream) = self.streams.get_mut(stream_id as usize) {
            let result = match target {
                PcmState::Prepared => {
                    let r = stream.prepare();
                    if r.is_ok() {
                        // Nothing backend-specific for prepare
                    }
                    r
                }
                PcmState::Running => {
                    let r = stream.start();
                    if r.is_ok() {
                        let _ = self.backend.start(stream_id);
                    }
                    r
                }
                PcmState::Stopped => {
                    let r = stream.stop();
                    if r.is_ok() {
                        let _ = self.backend.stop(stream_id);
                    }
                    r
                }
                PcmState::Released => stream.release(),
                _ => return cmd::VIRTIO_SND_S_BAD_MSG,
            };

            match result {
                Ok(()) => cmd::VIRTIO_SND_S_OK,
                Err(_) => cmd::VIRTIO_SND_S_IO_ERR,
            }
        } else {
            cmd::VIRTIO_SND_S_BAD_MSG
        }
    }

    /// Map a rate index (from SET_PARAMS) to Hz.
    fn rate_index_to_hz(index: u8) -> Option<u32> {
        match index {
            0 => Some(5512),
            1 => Some(8000),
            2 => Some(11025),
            3 => Some(16000),
            4 => Some(22050),
            5 => Some(32000),
            6 => Some(44100),
            7 => Some(48000),
            8 => Some(64000),
            9 => Some(88200),
            10 => Some(96000),
            11 => Some(176400),
            12 => Some(192000),
            _ => None,
        }
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    /// Get jack info by index.
    pub fn get_jack_info(&self, index: u32) -> Option<&JackInfo> {
        self.jacks.get(index as usize)
    }

    /// Get the number of jacks.
    pub fn num_jacks(&self) -> u32 {
        self.jacks.len() as u32
    }

    /// Get PCM info by stream index.
    pub fn get_pcm_info(&self, index: u32) -> Option<&PcmInfo> {
        self.pcm_infos.get(index as usize)
    }

    /// Get the number of PCM streams.
    pub fn num_streams(&self) -> u32 {
        self.streams.len() as u32
    }

    /// Get a PCM stream by index.
    pub fn get_stream(&self, index: u32) -> Option<&PcmStream> {
        self.streams.get(index as usize)
    }

    /// Get a mutable PCM stream by index.
    pub fn get_stream_mut(&mut self, index: u32) -> Option<&mut PcmStream> {
        self.streams.get_mut(index as usize)
    }

    /// Get channel map info by index.
    pub fn get_chmap_info(&self, index: u32) -> Option<&ChannelMapInfo> {
        self.chmaps.get(index as usize)
    }

    /// Get the number of channel maps.
    pub fn num_chmaps(&self) -> u32 {
        self.chmaps.len() as u32
    }

    // ========================================================================
    // Audio data path
    // ========================================================================

    /// Write playback data to a stream. Data flows: guest TX queue -> stream buffer -> backend.
    pub fn write_pcm_data(&mut self, stream_id: u32, data: &[u8]) -> DeviceResult<usize> {
        // First write to stream buffer
        if let Some(stream) = self.streams.get_mut(stream_id as usize) {
            let n = stream.write_samples(data)?;
            // Then forward to backend
            let _ = self.backend.write_samples(stream_id, &data[..n]);
            Ok(n)
        } else {
            Err(DeviceError::UnsupportedFeature(format!(
                "invalid stream ID {}",
                stream_id
            )))
        }
    }

    /// Read capture data from a stream. Data flows: backend -> stream buffer -> guest RX queue.
    pub fn read_pcm_data(&mut self, stream_id: u32, buf: &mut [u8]) -> DeviceResult<usize> {
        // Read from backend into temp buffer
        let mut backend_buf = vec![0u8; buf.len()];
        let backend_n = self.backend.read_samples(stream_id, &mut backend_buf)?;

        // Feed backend data into stream buffer
        if let Some(stream) = self.streams.get_mut(stream_id as usize) {
            if stream.state == PcmState::Running {
                stream.buffer.extend(&backend_buf[..backend_n]);
            }
            stream.read_samples(buf)
        } else {
            Err(DeviceError::UnsupportedFeature(format!(
                "invalid stream ID {}",
                stream_id
            )))
        }
    }

    // ========================================================================
    // Validation helpers
    // ========================================================================

    /// Validate that a sample format is supported
    pub fn is_format_supported(format: SampleFormat) -> bool {
        matches!(
            format,
            SampleFormat::U8 | SampleFormat::S16Le | SampleFormat::S32Le | SampleFormat::FloatLe
        )
    }

    /// Validate that a sample rate is supported
    pub fn is_rate_supported(rate: u32) -> bool {
        rates::ALL.contains(&rate)
    }
}

impl Default for VirtioSnd {
    fn default() -> Self {
        Self::new()
    }
}

impl VirtioDevice for VirtioSnd {
    fn device_type(&self) -> VirtioDeviceType {
        VirtioDeviceType::Sound
    }

    fn features(&self) -> u64 {
        self.features
    }

    fn activate(&mut self) -> DeviceResult<()> {
        self.activated = true;
        tracing::info!(
            "virtio-snd activated: {} jacks, {} streams, {} chmaps",
            self.jacks.len(),
            self.streams.len(),
            self.chmaps.len()
        );
        Ok(())
    }

    fn reset(&mut self) {
        self.activated = false;
        // Reset all streams to initial state
        for stream in &mut self.streams {
            stream.state = PcmState::Unset;
            stream.buffer.clear();
            stream.bytes_written = 0;
            stream.bytes_read = 0;
        }
        tracing::debug!("virtio-snd reset");
    }

    fn process_queue(&mut self, queue_index: u16) -> DeviceResult<()> {
        match queue_index {
            CONTROL_QUEUE => {
                tracing::trace!("virtio-snd: control queue notified");
                Ok(())
            }
            EVENT_QUEUE => {
                tracing::trace!("virtio-snd: event queue notified");
                Ok(())
            }
            TX_QUEUE => {
                tracing::trace!("virtio-snd: TX queue notified");
                Ok(())
            }
            RX_QUEUE => {
                tracing::trace!("virtio-snd: RX queue notified");
                Ok(())
            }
            _ => {
                tracing::warn!("virtio-snd: unknown queue index {}", queue_index);
                Ok(())
            }
        }
    }
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_device_creation_default() {
        let dev = VirtioSnd::new();
        assert_eq!(dev.device_type(), VirtioDeviceType::Sound);
        assert_eq!(dev.num_jacks(), 2);
        assert_eq!(dev.num_streams(), 2);
        assert_eq!(dev.num_chmaps(), 2);

        // Config space
        let config = dev.read_config();
        let jacks = u32::from_le_bytes([config[0], config[1], config[2], config[3]]);
        let streams = u32::from_le_bytes([config[4], config[5], config[6], config[7]]);
        let chmaps = u32::from_le_bytes([config[8], config[9], config[10], config[11]]);
        assert_eq!(jacks, 2);
        assert_eq!(streams, 2);
        assert_eq!(chmaps, 2);
    }

    #[test]
    fn test_pcm_info_query() {
        let dev = VirtioSnd::new();

        // Playback stream
        let info = dev.get_pcm_info(0).unwrap();
        assert_eq!(info.direction, StreamDirection::Playback);
        assert_eq!(info.channels_min, 1);
        assert_eq!(info.channels_max, 8);
        assert!(info.formats & SampleFormat::S16Le.to_bitmask() != 0);
        assert!(info.formats & SampleFormat::FloatLe.to_bitmask() != 0);
        assert!(info.supported_rates != 0);

        // Capture stream
        let info = dev.get_pcm_info(1).unwrap();
        assert_eq!(info.direction, StreamDirection::Capture);
        assert_eq!(info.channels_max, 2);

        // Invalid stream
        assert!(dev.get_pcm_info(99).is_none());
    }

    #[test]
    fn test_pcm_set_params() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        let stream = dev.get_stream_mut(0).unwrap();
        let params = PcmParams {
            buffer_bytes: 16384,
            period_bytes: 4096,
            channels: 2,
            format: SampleFormat::S16Le,
            rate: 48000,
        };
        stream.set_params(params).unwrap();

        let stream = dev.get_stream(0).unwrap();
        assert_eq!(stream.state, PcmState::SetParams);
        assert_eq!(stream.params.buffer_bytes, 16384);
        assert_eq!(stream.params.period_bytes, 4096);
        assert_eq!(stream.params.channels, 2);
        assert_eq!(stream.params.format, SampleFormat::S16Le);
        assert_eq!(stream.params.rate, 48000);
    }

    #[test]
    fn test_pcm_state_machine_full_cycle() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        let stream = dev.get_stream_mut(0).unwrap();
        assert_eq!(stream.state, PcmState::Unset);

        // SET_PARAMS
        stream.set_params(PcmParams::default()).unwrap();
        assert_eq!(stream.state, PcmState::SetParams);

        // PREPARE
        stream.prepare().unwrap();
        assert_eq!(stream.state, PcmState::Prepared);

        // START
        stream.start().unwrap();
        assert_eq!(stream.state, PcmState::Running);

        // STOP
        stream.stop().unwrap();
        assert_eq!(stream.state, PcmState::Stopped);

        // Can re-PREPARE from Stopped
        stream.prepare().unwrap();
        assert_eq!(stream.state, PcmState::Prepared);

        // RELEASE from Prepared
        stream.release().unwrap();
        assert_eq!(stream.state, PcmState::Released);
    }

    #[test]
    fn test_pcm_invalid_state_transitions() {
        let mut dev = VirtioSnd::new();

        let stream = dev.get_stream_mut(0).unwrap();
        assert_eq!(stream.state, PcmState::Unset);

        // Cannot PREPARE from Unset
        assert!(stream.prepare().is_err());

        // Cannot START from Unset
        assert!(stream.start().is_err());

        // Cannot STOP from Unset
        assert!(stream.stop().is_err());

        // Cannot RELEASE from Unset
        assert!(stream.release().is_err());

        // SET_PARAMS first, then try invalid transitions
        stream.set_params(PcmParams::default()).unwrap();

        // Cannot START from SetParams (must PREPARE first)
        assert!(stream.start().is_err());

        // Cannot STOP from SetParams
        assert!(stream.stop().is_err());

        // PREPARE, then try invalid
        stream.prepare().unwrap();

        // Cannot STOP from Prepared (must START first)
        assert!(stream.stop().is_err());

        // START, then try invalid
        stream.start().unwrap();

        // Cannot PREPARE from Running
        assert!(stream.prepare().is_err());

        // Cannot START from Running
        assert!(stream.start().is_err());

        // Cannot RELEASE from Running
        assert!(stream.release().is_err());
    }

    #[test]
    fn test_jack_info_query() {
        let dev = VirtioSnd::new();

        // Output jack
        let jack = dev.get_jack_info(0).unwrap();
        assert_eq!(jack.id, 0);
        assert!(jack.connected);
        assert_eq!(jack.hda_reg_defconf, 0x0121_1010);

        // Input jack
        let jack = dev.get_jack_info(1).unwrap();
        assert_eq!(jack.id, 1);
        assert!(jack.connected);
        assert_eq!(jack.hda_reg_defconf, 0x0181_3010);

        // Invalid jack
        assert!(dev.get_jack_info(99).is_none());
    }

    #[test]
    fn test_channel_map_info() {
        let dev = VirtioSnd::new();

        // Playback stereo channel map
        let chmap = dev.get_chmap_info(0).unwrap();
        assert_eq!(chmap.direction, StreamDirection::Playback);
        assert_eq!(chmap.channels, 2);
        assert_eq!(chmap.positions.len(), 2);
        assert_eq!(chmap.positions[0], ChannelPosition::FrontLeft);
        assert_eq!(chmap.positions[1], ChannelPosition::FrontRight);

        // Capture stereo channel map
        let chmap = dev.get_chmap_info(1).unwrap();
        assert_eq!(chmap.direction, StreamDirection::Capture);
        assert_eq!(chmap.channels, 2);

        // Invalid
        assert!(dev.get_chmap_info(99).is_none());
    }

    #[test]
    fn test_audio_format_validation() {
        // Valid formats
        assert!(VirtioSnd::is_format_supported(SampleFormat::U8));
        assert!(VirtioSnd::is_format_supported(SampleFormat::S16Le));
        assert!(VirtioSnd::is_format_supported(SampleFormat::S32Le));
        assert!(VirtioSnd::is_format_supported(SampleFormat::FloatLe));

        // Format parsing
        assert_eq!(SampleFormat::from_u8(0), Some(SampleFormat::U8));
        assert_eq!(SampleFormat::from_u8(2), Some(SampleFormat::S16Le));
        assert_eq!(SampleFormat::from_u8(6), Some(SampleFormat::S32Le));
        assert_eq!(SampleFormat::from_u8(8), Some(SampleFormat::FloatLe));
        assert_eq!(SampleFormat::from_u8(99), None);

        // Sample sizes
        assert_eq!(SampleFormat::U8.sample_bytes(), 1);
        assert_eq!(SampleFormat::S16Le.sample_bytes(), 2);
        assert_eq!(SampleFormat::S32Le.sample_bytes(), 4);
        assert_eq!(SampleFormat::FloatLe.sample_bytes(), 4);
    }

    #[test]
    fn test_sample_rate_validation() {
        assert!(VirtioSnd::is_rate_supported(8000));
        assert!(VirtioSnd::is_rate_supported(11025));
        assert!(VirtioSnd::is_rate_supported(16000));
        assert!(VirtioSnd::is_rate_supported(22050));
        assert!(VirtioSnd::is_rate_supported(44100));
        assert!(VirtioSnd::is_rate_supported(48000));
        assert!(VirtioSnd::is_rate_supported(96000));
        assert!(!VirtioSnd::is_rate_supported(12345));
        assert!(!VirtioSnd::is_rate_supported(0));
    }

    #[test]
    fn test_activate_reset_lifecycle() {
        let mut dev = VirtioSnd::new();

        // Activate
        dev.activate().unwrap();

        // Set up a stream
        let stream = dev.get_stream_mut(0).unwrap();
        stream.set_params(PcmParams::default()).unwrap();
        stream.prepare().unwrap();
        stream.start().unwrap();

        // Write some data
        let data = vec![0u8; 100];
        stream.write_samples(&data).unwrap();
        assert!(stream.bytes_written > 0);

        // Reset should clear all stream state
        dev.reset();

        let stream = dev.get_stream(0).unwrap();
        assert_eq!(stream.state, PcmState::Unset);
        assert_eq!(stream.bytes_written, 0);
        assert_eq!(stream.buffered_bytes(), 0);
    }

    #[test]
    fn test_pcm_data_write_read() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        // Set up playback stream
        let stream = dev.get_stream_mut(0).unwrap();
        stream.set_params(PcmParams {
            buffer_bytes: 1024,
            period_bytes: 256,
            channels: 2,
            format: SampleFormat::S16Le,
            rate: 48000,
        }).unwrap();
        stream.prepare().unwrap();
        stream.start().unwrap();

        // Write playback data
        let samples = vec![0x42u8; 256];
        let written = dev.write_pcm_data(0, &samples).unwrap();
        assert_eq!(written, 256);

        // Stream buffer should have data
        let stream = dev.get_stream(0).unwrap();
        assert_eq!(stream.buffered_bytes(), 256);
    }

    #[test]
    fn test_null_backend() {
        let mut backend = NullBackend::new();

        // Write drops audio
        let data = vec![0u8; 1024];
        let n = backend.write_samples(0, &data).unwrap();
        assert_eq!(n, 1024);
        assert_eq!(backend.bytes_dropped, 1024);

        // Read generates silence
        let mut buf = vec![0xFFu8; 512];
        let n = backend.read_samples(0, &mut buf).unwrap();
        assert_eq!(n, 512);
        assert!(buf.iter().all(|&b| b == 0));
        assert_eq!(backend.bytes_generated, 512);

        // Start/stop are no-ops
        backend.start(0).unwrap();
        backend.stop(0).unwrap();
    }

    #[test]
    fn test_process_command_set_params() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        // Build a SET_PARAMS command
        let mut cmd_data = Vec::new();
        // Header: code = VIRTIO_SND_R_PCM_SET_PARAMS
        cmd_data.extend_from_slice(&cmd::VIRTIO_SND_R_PCM_SET_PARAMS.to_le_bytes());
        // stream_id = 0
        cmd_data.extend_from_slice(&0u32.to_le_bytes());
        // buffer_bytes = 8192
        cmd_data.extend_from_slice(&8192u32.to_le_bytes());
        // period_bytes = 4096
        cmd_data.extend_from_slice(&4096u32.to_le_bytes());
        // channels = 2
        cmd_data.push(2);
        // format = S16_LE (index 2)
        cmd_data.push(2);
        // rate = 48000 (index 7)
        cmd_data.push(7);
        // padding
        cmd_data.extend_from_slice(&[0u8; 5]);

        let status = dev.process_command(&cmd_data);
        assert_eq!(status, cmd::VIRTIO_SND_S_OK);

        let stream = dev.get_stream(0).unwrap();
        assert_eq!(stream.state, PcmState::SetParams);
        assert_eq!(stream.params.buffer_bytes, 8192);
        assert_eq!(stream.params.rate, 48000);
    }

    #[test]
    fn test_process_command_transitions() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        // Helper to build transition command
        let make_cmd = |code: u32, stream_id: u32| -> Vec<u8> {
            let mut data = Vec::new();
            data.extend_from_slice(&code.to_le_bytes());
            data.extend_from_slice(&stream_id.to_le_bytes());
            data
        };

        // First set params
        let mut set_params = Vec::new();
        set_params.extend_from_slice(&cmd::VIRTIO_SND_R_PCM_SET_PARAMS.to_le_bytes());
        set_params.extend_from_slice(&0u32.to_le_bytes()); // stream_id
        set_params.extend_from_slice(&8192u32.to_le_bytes()); // buffer_bytes
        set_params.extend_from_slice(&4096u32.to_le_bytes()); // period_bytes
        set_params.push(2); // channels
        set_params.push(2); // format (S16_LE)
        set_params.push(7); // rate (48000)
        set_params.extend_from_slice(&[0u8; 5]); // padding
        assert_eq!(dev.process_command(&set_params), cmd::VIRTIO_SND_S_OK);

        // PREPARE
        assert_eq!(
            dev.process_command(&make_cmd(cmd::VIRTIO_SND_R_PCM_PREPARE, 0)),
            cmd::VIRTIO_SND_S_OK
        );
        assert_eq!(dev.get_stream(0).unwrap().state, PcmState::Prepared);

        // START
        assert_eq!(
            dev.process_command(&make_cmd(cmd::VIRTIO_SND_R_PCM_START, 0)),
            cmd::VIRTIO_SND_S_OK
        );
        assert_eq!(dev.get_stream(0).unwrap().state, PcmState::Running);

        // STOP
        assert_eq!(
            dev.process_command(&make_cmd(cmd::VIRTIO_SND_R_PCM_STOP, 0)),
            cmd::VIRTIO_SND_S_OK
        );
        assert_eq!(dev.get_stream(0).unwrap().state, PcmState::Stopped);

        // RELEASE
        assert_eq!(
            dev.process_command(&make_cmd(cmd::VIRTIO_SND_R_PCM_RELEASE, 0)),
            cmd::VIRTIO_SND_S_OK
        );
        assert_eq!(dev.get_stream(0).unwrap().state, PcmState::Released);
    }

    #[test]
    fn test_process_command_invalid() {
        let mut dev = VirtioSnd::new();

        // Too short
        assert_eq!(dev.process_command(&[0, 1]), cmd::VIRTIO_SND_S_BAD_MSG);

        // Unknown command
        assert_eq!(
            dev.process_command(&0xFFFFu32.to_le_bytes()),
            cmd::VIRTIO_SND_S_NOT_SUPP
        );

        // JACK_REMAP not supported
        assert_eq!(
            dev.process_command(&cmd::VIRTIO_SND_R_JACK_REMAP.to_le_bytes()),
            cmd::VIRTIO_SND_S_NOT_SUPP
        );
    }

    #[test]
    fn test_process_queue() {
        let mut dev = VirtioSnd::new();
        dev.activate().unwrap();

        assert!(dev.process_queue(CONTROL_QUEUE).is_ok());
        assert!(dev.process_queue(EVENT_QUEUE).is_ok());
        assert!(dev.process_queue(TX_QUEUE).is_ok());
        assert!(dev.process_queue(RX_QUEUE).is_ok());
        assert!(dev.process_queue(99).is_ok()); // unknown queue
    }

    #[test]
    fn test_surround_channel_map() {
        let chmap = ChannelMapInfo::surround_5_1(0, StreamDirection::Playback);
        assert_eq!(chmap.channels, 6);
        assert_eq!(chmap.positions.len(), 6);
        assert_eq!(chmap.positions[0], ChannelPosition::FrontLeft);
        assert_eq!(chmap.positions[1], ChannelPosition::FrontRight);
        assert_eq!(chmap.positions[2], ChannelPosition::FrontCenter);
        assert_eq!(chmap.positions[3], ChannelPosition::Lfe);
        assert_eq!(chmap.positions[4], ChannelPosition::RearLeft);
        assert_eq!(chmap.positions[5], ChannelPosition::RearRight);
    }

    #[test]
    fn test_snd_hdr_serialization() {
        let hdr = SndHdr { code: cmd::VIRTIO_SND_R_PCM_INFO };
        let bytes = hdr.to_bytes();
        let parsed = SndHdr::from_bytes(&bytes).unwrap();
        assert_eq!(parsed.code, cmd::VIRTIO_SND_R_PCM_INFO);

        // Too short
        assert!(SndHdr::from_bytes(&[0, 1]).is_err());
    }

    #[test]
    fn test_stream_buffer_capacity() {
        let mut stream = PcmStream::new(0, StreamDirection::Playback);
        stream.set_params(PcmParams {
            buffer_bytes: 64,
            period_bytes: 32,
            channels: 1,
            format: SampleFormat::U8,
            rate: 8000,
        }).unwrap();
        stream.prepare().unwrap();
        stream.start().unwrap();

        // Write exactly buffer_bytes
        let data = vec![0xAB; 64];
        let n = stream.write_samples(&data).unwrap();
        assert_eq!(n, 64);
        assert_eq!(stream.buffered_bytes(), 64);

        // Buffer is full, should write 0 bytes
        let more = vec![0xCD; 32];
        let n = stream.write_samples(&more).unwrap();
        assert_eq!(n, 0);

        // Read some data
        let mut buf = vec![0u8; 32];
        let n = stream.read_samples(&mut buf).unwrap();
        assert_eq!(n, 32);
        assert!(buf.iter().all(|&b| b == 0xAB));

        // Now can write again
        let n = stream.write_samples(&more).unwrap();
        assert_eq!(n, 32);
    }

    #[test]
    fn test_default_trait() {
        let dev = VirtioSnd::default();
        assert_eq!(dev.device_type(), VirtioDeviceType::Sound);
        assert_eq!(dev.num_streams(), 2);
    }

    #[test]
    fn test_rate_bits() {
        // Verify all supported rates have valid bits
        for &rate in rates::ALL {
            assert!(
                rates::rate_bit(rate).is_some(),
                "rate {} should have a rate bit",
                rate
            );
        }
        // Unsupported rate returns None
        assert!(rates::rate_bit(12345).is_none());
    }
}
