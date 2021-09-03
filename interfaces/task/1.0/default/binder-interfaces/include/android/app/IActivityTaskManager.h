#ifndef AIDL_GENERATED_ANDROID_APP_I_ACTIVITY_TASK_MANAGER_H_
#define AIDL_GENERATED_ANDROID_APP_I_ACTIVITY_TASK_MANAGER_H_

#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/Status.h>
#include <cstdint>
#include <utils/StrongPointer.h>

namespace android {

namespace app {

class IActivityTaskManager : public ::android::IInterface {
public:
  DECLARE_META_INTERFACE(ActivityTaskManager)
  virtual ::android::binder::Status removeTask(int32_t taskId, bool* _aidl_return) = 0;
  virtual ::android::binder::Status removeAllVisibleRecentTasks() = 0;
};  // class IActivityTaskManager

class IActivityTaskManagerDefault : public IActivityTaskManager {
public:
  ::android::IBinder* onAsBinder() override;
  ::android::binder::Status removeTask(int32_t taskId, bool* _aidl_return) override;
  ::android::binder::Status removeAllVisibleRecentTasks() override;

};

}  // namespace app

}  // namespace android

#endif  // AIDL_GENERATED_ANDROID_APP_I_ACTIVITY_TASK_MANAGER_H_
