#ifndef HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_2_BPHWWAYDROIDDISPLAY_H
#define HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_2_BPHWWAYDROIDDISPLAY_H

#include <hidl/HidlTransportSupport.h>

#include <vendor/waydroid/display/1.2/IHwWaydroidDisplay.h>

#include <mutex>
namespace vendor {
namespace waydroid {
namespace display {
namespace V1_2 {

struct BpHwWaydroidDisplay : public ::android::hardware::BpInterface<IWaydroidDisplay>, public ::android::hardware::details::HidlInstrumentor {
    explicit BpHwWaydroidDisplay(const ::android::sp<::android::hardware::IBinder> &_hidl_impl);

    /**
     * The pure class is what this class wraps.
     */
    typedef IWaydroidDisplay Pure;

    /**
     * Type tag for use in template logic that indicates this is a 'proxy' class.
     */
    typedef ::android::hardware::details::bphw_tag _hidl_tag;

    virtual bool isRemote() const override { return true; }

    void onLastStrongRef(const void* id) override;

    // Methods from ::vendor::waydroid::display::V1_2::IWaydroidDisplay follow.
    static ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error>  _hidl_setMouseMetadata(::android::hardware::IInterface* _hidl_this, ::android::hardware::details::HidlInstrumentor *_hidl_this_instrumentor, uint32_t layer, int32_t style, float hotspotX, float hotspotY);

    // Methods from ::vendor::waydroid::display::V1_0::IWaydroidDisplay follow.
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerName(uint32_t layer, const ::android::hardware::hidl_string& name) override;
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerHandleInfo(uint32_t layer, uint32_t format, uint32_t stride) override;
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setTargetLayerHandleInfo(uint32_t format, uint32_t stride) override;

    // Methods from ::vendor::waydroid::display::V1_1::IWaydroidDisplay follow.
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setLayerSize(uint32_t layer, uint32_t width, uint32_t height) override;
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setTargetLayerSize(uint32_t width, uint32_t height) override;

    // Methods from ::vendor::waydroid::display::V1_2::IWaydroidDisplay follow.
    ::android::hardware::Return<::android::hardware::graphics::composer::V2_1::Error> setMouseMetadata(uint32_t layer, int32_t style, float hotspotX, float hotspotY) override;

    // Methods from ::android::hidl::base::V1_0::IBase follow.
    ::android::hardware::Return<void> interfaceChain(interfaceChain_cb _hidl_cb) override;
    ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle& fd, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>& options) override;
    ::android::hardware::Return<void> interfaceDescriptor(interfaceDescriptor_cb _hidl_cb) override;
    ::android::hardware::Return<void> getHashChain(getHashChain_cb _hidl_cb) override;
    ::android::hardware::Return<void> setHALInstrumentation() override;
    ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient, uint64_t cookie) override;
    ::android::hardware::Return<void> ping() override;
    ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb) override;
    ::android::hardware::Return<void> notifySyspropsChanged() override;
    ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>& recipient) override;

private:
    std::mutex _hidl_mMutex;
    std::vector<::android::sp<::android::hardware::hidl_binder_death_recipient>> _hidl_mDeathRecipients;
};

}  // namespace V1_2
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_2_BPHWWAYDROIDDISPLAY_H
