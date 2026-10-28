  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF Binaries/mars/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF Binaries/mars/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
  INF Binaries/mars/QcomPkg/Drivers/TzDxe/ScmDxeLA_Kailua.inf
  INF Binaries/mars/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/mars/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
  #INF Binaries/mars/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/mars/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
  INF Binaries/mars/StarPkg/Drivers/StarHwIdDxe/HwIdDxe.inf
  INF Binaries/mars/StarPkg/Drivers/StarDxe/StarDxe.inf

  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  # The XNU loader is part of the firmware: a driver loaded from a volume is
  # refused by the image verification of this build.
  INF DarwinBootPkg/Driver/DarwinBoot.inf
  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/mars/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/mars/QcomPkg/Drivers/RscDxe/RscDxe.inf

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/mars/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/mars/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/mars/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/mars/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/mars/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/mars/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/mars/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/UCDxe/UCDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/RNGDxe/RngDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/mars/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
