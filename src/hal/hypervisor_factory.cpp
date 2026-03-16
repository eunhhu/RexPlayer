#include "rex/hal/hypervisor.h"

#ifdef REX_HAS_KVM
#include "kvm/kvm_hypervisor.h"
#endif
#ifdef REX_HAS_WHPX
#include "whpx/whpx_hypervisor.h"
#endif
#ifdef REX_HAS_HVF
#if defined(__aarch64__)
#include "hvf/hvf_arm64_adapter.h"
#else
#include "hvf/hvf_hypervisor.h"
#endif
#endif

#include <stdexcept>

namespace rex::hal {

std::unique_ptr<IHypervisor> create_hypervisor() {
#ifdef REX_HAS_KVM
    auto hv = std::make_unique<KvmHypervisor>();
    if (hv->is_available()) return hv;
#endif
#ifdef REX_HAS_WHPX
    auto hv = std::make_unique<WhpxHypervisor>();
    if (hv->is_available()) return hv;
#endif
#ifdef REX_HAS_HVF
#if defined(__aarch64__)
    auto hv = std::make_unique<HvfArm64Hypervisor>();
#else
    auto hv = std::make_unique<HvfHypervisor>();
#endif
    if (hv->is_available()) return hv;
#endif
    throw std::runtime_error("No supported hypervisor found on this system");
}

} // namespace rex::hal
