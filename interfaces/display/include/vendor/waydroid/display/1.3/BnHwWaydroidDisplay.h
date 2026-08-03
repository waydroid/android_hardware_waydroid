#ifndef HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_3_BNHWWAYDROIDDISPLAY_H
#define HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_3_BNHWWAYDROIDDISPLAY_H

#include <vendor/waydroid/display/1.3/IHwWaydroidDisplay.h>

namespace vendor {
namespace waydroid {
namespace display {
namespace V1_3 {

struct BnHwWaydroidDisplay : public ::android::hidl::base::V1_0::BnHwBase {
    explicit BnHwWaydroidDisplay(const ::android::sp<IWaydroidDisplay> &_hidl_impl);
    explicit BnHwWaydroidDisplay(const ::android::sp<IWaydroidDisplay> &_hidl_impl, const std::string& HidlInstrumentor_package, const std::string& HidlInstrumentor_interface);

    virtual ~BnHwWaydroidDisplay();

    ::android::status_t onTransact(
            uint32_t _hidl_code,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            uint32_t _hidl_flags = 0,
            TransactCallback _hidl_cb = nullptr) override;


    /**
     * The pure class is what this class wraps.
     */
    typedef IWaydroidDisplay Pure;

    /**
     * Type tag for use in template logic that indicates this is a 'native' class.
     */
    typedef ::android::hardware::details::bnhw_tag _hidl_tag;

    ::android::sp<IWaydroidDisplay> getImpl() { return _hidl_mImpl; }
    // Methods from ::vendor::waydroid::display::V1_3::IWaydroidDisplay follow.
    static ::android::status_t _hidl_postTaskBuffer(
            ::android::hidl::base::V1_0::BnHwBase* _hidl_this,
            const ::android::hardware::Parcel &_hidl_data,
            ::android::hardware::Parcel *_hidl_reply,
            TransactCallback _hidl_cb);



private:
    // Methods from ::vendor::waydroid::display::V1_0::IWaydroidDisplay follow.

    // Methods from ::vendor::waydroid::display::V1_1::IWaydroidDisplay follow.

    // Methods from ::vendor::waydroid::display::V1_2::IWaydroidDisplay follow.

    // Methods from ::vendor::waydroid::display::V1_3::IWaydroidDisplay follow.

    // Methods from ::android::hidl::base::V1_0::IBase follow.
    ::android::hardware::Return<void> ping();
    using getDebugInfo_cb = ::android::hidl::base::V1_0::IBase::getDebugInfo_cb;
    ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb _hidl_cb);

    ::android::sp<IWaydroidDisplay> _hidl_mImpl;
};

}  // namespace V1_3
}  // namespace display
}  // namespace waydroid
}  // namespace vendor

#endif  // HIDL_GENERATED_VENDOR_WAYDROID_DISPLAY_V1_3_BNHWWAYDROIDDISPLAY_H
