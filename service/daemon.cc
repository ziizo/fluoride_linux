//
//  Copyright 2015 Google, Inc.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at:
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#include "service/daemon.h"

#include <memory>

#include <base/logging.h>
#include <base/run_loop.h>
#include <hardware/bt_hearing_aid.h>

#include "service/adapter.h"
#include "service/hal/bluetooth_av_interface.h"
#include "service/hal/bluetooth_avrcp_interface.h"
#include "service/hal/bluetooth_gatt_interface.h"
#include "service/hal/bluetooth_interface.h"
#include "service/ipc/ipc_manager.h"
#include "service/settings.h"
#include "service/switches.h"
using bluetooth::hearing_aid::ConnectionState;
using bluetooth::hearing_aid::HearingAidInterface;
//extern HearingAidInterface* btif_hearing_aid_get_interface();

namespace bluetooth {

namespace {
const std::string DesiredDeviceName = "nimble-bleprph"; //Synopsys_BLE_Dev  OPPO A57 nimble-bleprph nimble_server
const std::string DesiredDeviceAddress = "xx:xx:xx:xx:xx:xx";

// The global Daemon instance.
Daemon* g_daemon = nullptr;

class DaemonImpl : public Daemon, public ipc::IPCManager::Delegate ,
					public bluetooth::Adapter::Observer ,
					public bluetooth::hearing_aid::HearingAidCallbacks{
 public:
  DaemonImpl() : initialized_(false) {
  }

  ~DaemonImpl() override {
    if (!initialized_) return;

    CleanUpBluetoothStack();
  }

  void StartMainLoop() override { base::RunLoop().Run(); }
  bool SetScanEnable(bool scan_enable) override  {
	 return  adapter_->SetScanEnable(scan_enable);
  }
  bool CreateBond(const std::string& device_address, int transport) override {
	  return adapter_->CreateBond(device_address,transport);
  }
  virtual void OnConnectionState(ConnectionState state,
                                 const RawAddress& address) override {
	  LOG(INFO) << "DEVICE IS  " << (int)state ;
  }


  virtual void OnDeviceAvailable(uint8_t capabilities, uint64_t hiSyncId,
                                 const RawAddress& address) override {
	  LOG(INFO) << "DEVICE IS available with address " << address ;
  }

  Settings* GetSettings() const override { return settings_.get(); }

  base::MessageLoop* GetMessageLoop() const override {
    return message_loop_.get();
  }

 private:
  hearing_aid::HearingAidInterface* HearingAid_iface = nullptr;

  // ipc::IPCManager::Delegate implementation:
  void OnIPCHandlerStarted(ipc::IPCManager::Type /* type */) override {
    if (!settings_->EnableOnStart()) return;
    adapter_->Enable(false /* start_restricted */);
    LOG(INFO) << "enabled";
  }

  void OnIPCHandlerStopped(ipc::IPCManager::Type /* type */) override {
    // Do nothing.
  }

  bool StartUpBluetoothInterfaces() {
    if (!hal::BluetoothInterface::Initialize()) goto failed;

    if (!hal::BluetoothGattInterface::Initialize()) goto failed;


//    if (!hal::BluetoothHaInterface::Initialize()) goto failed;

//    if (!hal::BluetoothAvrcpInterface::Initialize()) goto failed;

    return true;

  failed:
    ShutDownBluetoothInterfaces();
    return false;
  }

  void ShutDownBluetoothInterfaces() {
    if (hal::BluetoothGattInterface::IsInitialized())
      hal::BluetoothGattInterface::CleanUp();
    if (hal::BluetoothInterface::IsInitialized())
      hal::BluetoothInterface::CleanUp();
    if (hal::BluetoothAvInterface::IsInitialized())
      hal::BluetoothAvInterface::CleanUp();
    if (hal::BluetoothAvrcpInterface::IsInitialized())
      hal::BluetoothAvrcpInterface::CleanUp();
  }

  void CleanUpBluetoothStack() {
    // The Adapter object needs to be cleaned up before the HAL interfaces.
    ipc_manager_.reset();
    adapter_.reset();
    ShutDownBluetoothInterfaces();
  }

  bool SetUpIPC() {
    // If an IPC socket path was given, initialize IPC with it. Otherwise
    // initialize Binder IPC.
    if (settings_->UseSocketIPC()) {
      if (!ipc_manager_->Start(ipc::IPCManager::TYPE_LINUX, this)) {
        LOG(ERROR) << "Failed to set up UNIX domain-socket IPCManager";
        return false;
      }
      return true;
    }

#if !defined(OS_GENERIC)
    if (!ipc_manager_->Start(ipc::IPCManager::TYPE_BINDER, this)) {
      LOG(ERROR) << "Failed to set up Binder IPCManager";
      return false;
    }
#else
    if (!ipc_manager_->Start(ipc::IPCManager::TYPE_DBUS, this)) {
      LOG(ERROR) << "Failed to set up DBus IPCManager";
      return false;
    }
#endif

    return true;
  }

  bool Init() override {
    CHECK(!initialized_);
    message_loop_.reset(new base::MessageLoop());

    settings_.reset(new Settings());
    if (!settings_->Init()) {
      LOG(ERROR) << "Failed to set up Settings";
      return false;
    }
    LOG(INFO) << "after setting init";
    if (!StartUpBluetoothInterfaces()) {
      LOG(ERROR) << "Failed to set up HAL Bluetooth interfaces";
      return false;
    }
    LOG(INFO) << "after start up bt";
    adapter_ = Adapter::Create();
    adapter_->AddObserver(this);
    ipc_manager_.reset(new ipc::IPCManager(adapter_.get()));

    if (!SetUpIPC()) {
      CleanUpBluetoothStack();
      return false;
    }

    initialized_ = true;
    LOG(INFO) << "Daemon initialized";
    HearingAid_iface = (hearing_aid::HearingAidInterface*)(
        		hal::BluetoothInterface::Get()->GetHALInterface()->get_profile_interface(
        				BT_PROFILE_HEARING_AID_ID));
    LOG(INFO) << "AFTER GET HA ID";

    if(!HearingAid_iface){
        return false;
    }
    return true;
  }

  void OnAdapterStateChanged(Adapter* adapter,
                              AdapterState prev_state,
                              AdapterState new_state) override {
  	LOG(INFO) << "state changed Daemon impl";
  	if (new_state == ADAPTER_STATE_ON){
	    if(!SetScanEnable(true)){
		    LOG(ERROR) << "CANNOT START SCANNING"; //this found on the log
	    };
	    HearingAid_iface->Init(this);
  	}
    // Default implementation does nothing
  }
  void OnDeviceFound(Adapter* adapter,
                     const RemoteDeviceProps& props) override {
	  	LOG(INFO) << "device found Daemon impl";
	  	LOG(INFO) << "Device address: " << props.address();
	  	LOG(INFO) << "Device name: " << props.name();
	  	if(props.name() == DesiredDeviceName){
	    LOG(INFO) << "the desired device found"; //this found on the log
	    if(!SetScanEnable(false)){
		    LOG(ERROR) << "CANNOT STOP SCANNING"; //this found on the log
	    };
//	    if(!CreateBond(props.address(),2)){
//		    LOG(ERROR) << "CANNOT CONNECT WITH THE SPECIFIC DEVICE ADDRESS"; //this found on the log
//	    }
        RawAddress bd_addr;
        RawAddress::FromString(props.address(),bd_addr);
	    HearingAid_iface->Connect(bd_addr);

	  	}
  }
  bool initialized_;
  std::unique_ptr<base::MessageLoop> message_loop_;
  std::unique_ptr<Settings> settings_;
  std::unique_ptr<Adapter> adapter_;
  std::unique_ptr<ipc::IPCManager> ipc_manager_;

  DISALLOW_COPY_AND_ASSIGN(DaemonImpl);


};


}  // namespace
// static
bool Daemon::Initialize() {
  CHECK(!g_daemon);

  g_daemon = new DaemonImpl();
  if (g_daemon->Init()) return true;
  LOG(ERROR) << "Failed to initialize the Daemon object";

  delete g_daemon;
  g_daemon = nullptr;

  return false;
}

void Daemon::ShutDown() {
  CHECK(g_daemon);
  delete g_daemon;
  g_daemon = nullptr;
}

// static
void Daemon::InitializeForTesting(Daemon* test_daemon) {
  CHECK(test_daemon);
  CHECK(!g_daemon);

  g_daemon = test_daemon;
}

// static
Daemon* Daemon::Get() {
  CHECK(g_daemon);
  return g_daemon;
}

}  // namespace bluetooth
