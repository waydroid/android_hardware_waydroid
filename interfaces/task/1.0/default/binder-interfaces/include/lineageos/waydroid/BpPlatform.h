#ifndef AIDL_GENERATED_LINEAGEOS_WAYDROID_BP_PLATFORM_H_
#define AIDL_GENERATED_LINEAGEOS_WAYDROID_BP_PLATFORM_H_

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <utils/Errors.h>
#include <lineageos/waydroid/IPlatform.h>

namespace lineageos {

namespace waydroid {

class BpPlatform : public ::android::BpInterface<IPlatform> {
public:
  explicit BpPlatform(const ::android::sp<::android::IBinder>& _aidl_impl);
  virtual ~BpPlatform() = default;
  ::android::binder::Status getAppName(const ::android::String16& packageName, ::android::String16* _aidl_return) override;
};  // class BpPlatform

}  // namespace waydroid

}  // namespace lineageos

#endif  // AIDL_GENERATED_LINEAGEOS_WAYDROID_BP_PLATFORM_H_
