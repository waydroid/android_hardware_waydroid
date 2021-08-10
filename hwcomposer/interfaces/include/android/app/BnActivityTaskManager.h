#ifndef AIDL_GENERATED_ANDROID_APP_BN_ACTIVITY_TASK_MANAGER_H_
#define AIDL_GENERATED_ANDROID_APP_BN_ACTIVITY_TASK_MANAGER_H_

#include <binder/IInterface.h>
#include <android/app/IActivityTaskManager.h>

namespace android {

namespace app {

class BnActivityTaskManager : public ::android::BnInterface<IActivityTaskManager> {
public:
  ::android::status_t onTransact(uint32_t _aidl_code, const ::android::Parcel& _aidl_data, ::android::Parcel* _aidl_reply, uint32_t _aidl_flags) override;
};  // class BnActivityTaskManager

}  // namespace app

}  // namespace android

#endif  // AIDL_GENERATED_ANDROID_APP_BN_ACTIVITY_TASK_MANAGER_H_
