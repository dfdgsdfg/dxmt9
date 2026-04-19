#include "device_c_provider_api.hpp"

extern "C" D9CFactory* dxmt9c_factory_create(void) {
  return dxmt9p_factory_create();
}

extern "C" void dxmt9c_factory_addref(D9CFactory* arg0) {
  dxmt9p_factory_addref(arg0);
}

extern "C" uint32_t dxmt9c_factory_release(D9CFactory* arg0) {
  return dxmt9p_factory_release(arg0);
}

extern "C" uint32_t dxmt9c_factory_adapter_count(D9CFactory* arg0) {
  return dxmt9p_factory_adapter_count(arg0);
}

extern "C" int32_t dxmt9c_factory_get_adapter_identifier(D9CFactory* arg0, uint32_t adapter, D9CAdapterIdentifier* out) {
  return dxmt9p_factory_get_adapter_identifier(arg0, adapter, out);
}

extern "C" uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory* arg0, uint32_t adapter, uint32_t fmt) {
  return dxmt9p_factory_get_adapter_mode_count(arg0, adapter, fmt);
}

extern "C" int32_t dxmt9c_factory_enum_adapter_modes(D9CFactory* arg0, uint32_t adapter, uint32_t fmt, uint32_t mode, uint32_t* outW, uint32_t* outH, uint32_t* outRefresh, uint32_t* outFmt) {
  return dxmt9p_factory_enum_adapter_modes(arg0, adapter, fmt, mode, outW, outH, outRefresh, outFmt);
}

extern "C" int32_t dxmt9c_factory_get_adapter_display_mode(D9CFactory* arg0, uint32_t adapter, uint32_t* outW, uint32_t* outH, uint32_t* outRefresh, uint32_t* outFmt) {
  return dxmt9p_factory_get_adapter_display_mode(arg0, adapter, outW, outH, outRefresh, outFmt);
}

extern "C" uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory* arg0, uint32_t adapter) {
  return dxmt9p_factory_get_adapter_monitor(arg0, adapter);
}

extern "C" int32_t dxmt9c_factory_check_device_type(D9CFactory* arg0, uint32_t adapter, uint32_t devType, uint32_t adapterFmt, uint32_t backFmt, uint32_t windowed) {
  return dxmt9p_factory_check_device_type(arg0, adapter, devType, adapterFmt, backFmt, windowed);
}

extern "C" int32_t dxmt9c_factory_check_device_format(D9CFactory* arg0, uint32_t adapter, uint32_t fmt, uint32_t usage) {
  return dxmt9p_factory_check_device_format(arg0, adapter, fmt, usage);
}

extern "C" int32_t dxmt9c_factory_check_device_multisample(D9CFactory* arg0, uint32_t adapter, uint32_t fmt, uint32_t msType, uint32_t windowed) {
  return dxmt9p_factory_check_device_multisample(arg0, adapter, fmt, msType, windowed);
}

extern "C" int32_t dxmt9c_factory_get_caps(D9CFactory* arg0, uint32_t adapter, D9CCaps* out) {
  return dxmt9p_factory_get_caps(arg0, adapter, out);
}

extern "C" int32_t dxmt9c_factory_get_adapter_luid(D9CFactory* arg0, uint32_t adapter, uint32_t* lowPart, int32_t* highPart) {
  return dxmt9p_factory_get_adapter_luid(arg0, adapter, lowPart, highPart);
}

extern "C" D9CDevice* dxmt9c_factory_create_device(D9CFactory* arg0, uint32_t adapter, const D9CPresentParams* arg2, uint32_t behaviorFlags, const D9CDisplayModeEx* fullscreenMode) {
  return dxmt9p_factory_create_device(arg0, adapter, arg2, behaviorFlags, fullscreenMode);
}
