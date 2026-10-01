use rex_input_core::{
    AndroidSize, BoundsPolicy, ConfigError, CoordinateMapping, MappingError, PixelPoint, Rotation,
    Viewport,
};

fn mapping(rotation: Rotation, bounds: BoundsPolicy) -> CoordinateMapping {
    CoordinateMapping::new(
        Viewport::new(10.0, 20.0, 200.0, 100.0).unwrap(),
        AndroidSize::new(101, 201).unwrap(),
        rotation,
        bounds,
    )
}

#[test]
fn all_rotations_map_all_four_corners_and_an_asymmetric_point() {
    let cases = [
        (
            Rotation::None,
            [(0, 0), (100, 0), (100, 200), (0, 200)],
            (25, 150),
        ),
        (
            Rotation::Clockwise90,
            [(100, 0), (100, 200), (0, 200), (0, 0)],
            (25, 50),
        ),
        (
            Rotation::Clockwise180,
            [(100, 200), (0, 200), (0, 0), (100, 0)],
            (75, 50),
        ),
        (
            Rotation::Clockwise270,
            [(0, 200), (0, 0), (100, 0), (100, 200)],
            (75, 150),
        ),
    ];
    for (rotation, corners, interior) in cases {
        let map = mapping(rotation, BoundsPolicy::Reject);
        for ((u, v), (x, y)) in [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
            .into_iter()
            .zip(corners)
        {
            assert_eq!(map.map_normalized(u, v), Ok(PixelPoint { x, y }));
            assert_eq!(
                map.map_viewport(10.0 + 200.0 * u, 20.0 + 100.0 * v),
                Ok(PixelPoint { x, y }),
            );
        }
        assert_eq!(
            map.map_normalized(0.25, 0.75),
            Ok(PixelPoint {
                x: interior.0,
                y: interior.1
            }),
        );
    }
}

#[test]
fn centered_points_and_half_pixel_ties_round_up() {
    let map = CoordinateMapping::new(
        Viewport::new(0.0, 0.0, 10.0, 10.0).unwrap(),
        AndroidSize::new(1080, 1920).unwrap(),
        Rotation::None,
        BoundsPolicy::Reject,
    );
    assert_eq!(
        map.map_viewport(5.0, 5.0),
        Ok(PixelPoint { x: 540, y: 960 })
    );
    assert_eq!(
        map.map_viewport(10.0, 10.0),
        Ok(PixelPoint { x: 1079, y: 1919 })
    );
}

#[test]
fn reject_and_clamp_policies_have_explicit_edge_behavior() {
    let reject = mapping(Rotation::None, BoundsPolicy::Reject);
    let clamp = mapping(Rotation::None, BoundsPolicy::Clamp);
    for (x, y) in [(9.0, 20.0), (211.0, 20.0), (10.0, 19.0), (10.0, 121.0)] {
        assert_eq!(
            reject.map_viewport(x, y),
            Err(MappingError::OutsideViewport)
        );
    }
    assert_eq!(
        clamp.map_viewport(-f64::MAX, f64::MAX),
        Ok(PixelPoint { x: 0, y: 200 })
    );
    assert_eq!(
        clamp.map_viewport(f64::MAX, -f64::MAX),
        Ok(PixelPoint { x: 100, y: 0 })
    );
    for (u, v) in [(-0.01, 0.0), (1.01, 0.0), (0.0, -0.01), (0.0, 1.01)] {
        assert_eq!(
            reject.map_normalized(u, v),
            Err(MappingError::OutsideNormalizedBounds)
        );
    }
    assert_eq!(
        clamp.map_normalized(-f64::MAX, f64::MAX),
        Ok(PixelPoint { x: 0, y: 200 })
    );
    assert_eq!(
        clamp.map_normalized(f64::MAX, -f64::MAX),
        Ok(PixelPoint { x: 100, y: 0 })
    );
    assert_eq!(
        reject.map_normalized(-0.0, 1.0),
        Ok(PixelPoint { x: 0, y: 200 })
    );
}

#[test]
fn nonfinite_points_are_never_accepted_even_when_clamping() {
    for bounds in [BoundsPolicy::Reject, BoundsPolicy::Clamp] {
        let map = mapping(Rotation::None, bounds);
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
            for (x, y) in [(bad, 0.5), (0.5, bad)] {
                assert_eq!(map.map_normalized(x, y), Err(MappingError::NonFinitePoint));
                assert_eq!(map.map_viewport(x, y), Err(MappingError::NonFinitePoint));
            }
        }
    }
}

#[test]
fn invalid_geometry_is_rejected_at_construction() {
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        for (left, top, width, height) in [
            (bad, 0.0, 1.0, 1.0),
            (0.0, bad, 1.0, 1.0),
            (0.0, 0.0, bad, 1.0),
            (0.0, 0.0, 1.0, bad),
        ] {
            assert_eq!(
                Viewport::new(left, top, width, height),
                Err(ConfigError::InvalidViewport)
            );
        }
    }
    for (width, height) in [(0.0, 1.0), (1.0, 0.0), (-1.0, 1.0), (1.0, -1.0)] {
        assert_eq!(
            Viewport::new(0.0, 0.0, width, height),
            Err(ConfigError::InvalidViewport)
        );
    }
    assert_eq!(
        Viewport::new(f64::MAX, 0.0, f64::MAX, 1.0),
        Err(ConfigError::InvalidViewport)
    );
    assert_eq!(
        Viewport::new(0.0, f64::MAX, 1.0, f64::MAX),
        Err(ConfigError::InvalidViewport)
    );
    assert_eq!(
        Viewport::new(1e30, 0.0, 1.0, 1.0),
        Err(ConfigError::InvalidViewport)
    );
    assert_eq!(
        Viewport::new(0.0, 1e30, 1.0, 1.0),
        Err(ConfigError::InvalidViewport)
    );
    for (width, height) in [(0, 1), (1, 0), (u32::MAX, 1), (1, u32::MAX)] {
        assert_eq!(
            AndroidSize::new(width, height),
            Err(ConfigError::InvalidAndroidSize)
        );
    }
}

#[test]
fn one_pixel_and_maximum_android_dimensions_remain_in_bounds() {
    let viewport = Viewport::new(0.0, 0.0, 1.0, 1.0).unwrap();
    for rotation in [
        Rotation::None,
        Rotation::Clockwise90,
        Rotation::Clockwise180,
        Rotation::Clockwise270,
    ] {
        let single = CoordinateMapping::new(
            viewport,
            AndroidSize::new(1, 1).unwrap(),
            rotation,
            BoundsPolicy::Clamp,
        );
        for (x, y) in [(0.0, 0.0), (1.0, 1.0), (0.2, 0.9), (-10.0, 40.0)] {
            assert_eq!(single.map_normalized(x, y), Ok(PixelPoint { x: 0, y: 0 }));
        }
    }
    let size = AndroidSize::new(i32::MAX as u32, i32::MAX as u32).unwrap();
    assert_eq!(size.width(), i32::MAX as u32);
    assert_eq!(size.height(), i32::MAX as u32);
    let map = CoordinateMapping::new(viewport, size, Rotation::None, BoundsPolicy::Reject);
    assert_eq!(
        map.map_normalized(1.0, 1.0).unwrap(),
        PixelPoint {
            x: size.width() - 1,
            y: size.height() - 1
        }
    );
}

#[test]
fn large_origins_and_tiny_extents_map_representable_edges_exactly() {
    for (origin, width) in [
        (1e16, 2.9),
        (-1e16, 2.9),
        (-f64::MAX, f64::MAX),
        (0.0, f64::MIN_POSITIVE),
        (0.0, f64::from_bits(1)),
    ] {
        let map = CoordinateMapping::new(
            Viewport::new(origin, origin, width, width).unwrap(),
            AndroidSize::new(101, 201).unwrap(),
            Rotation::None,
            BoundsPolicy::Reject,
        );
        assert_eq!(
            map.map_viewport(origin, origin),
            Ok(PixelPoint { x: 0, y: 0 })
        );
        assert_eq!(
            map.map_viewport(origin + width, origin + width),
            Ok(PixelPoint { x: 100, y: 200 })
        );
    }
}

#[test]
fn coordinate_grid_stays_bounded_for_every_rotation() {
    for rotation in [
        Rotation::None,
        Rotation::Clockwise90,
        Rotation::Clockwise180,
        Rotation::Clockwise270,
    ] {
        let map = mapping(rotation, BoundsPolicy::Clamp);
        for x in -50..=150 {
            for y in -50..=150 {
                let point = map
                    .map_normalized(f64::from(x) / 100.0, f64::from(y) / 100.0)
                    .unwrap();
                assert!(point.x < 101 && point.y < 201);
            }
        }
    }
}
