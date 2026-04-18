#pragma once

#include "com_types.hpp"

#include <vector>

namespace dxmt9::util {

enum class ComPrivateDataType {
  None,
  Data,
  Iface,
};

class ComPrivateDataEntry {
 public:
  ComPrivateDataEntry();
  ComPrivateDataEntry(REFGUID guid, UINT size, const void* data);
  ComPrivateDataEntry(REFGUID guid, const IUnknown* iface);
  ~ComPrivateDataEntry();

  ComPrivateDataEntry(ComPrivateDataEntry&& other) noexcept;
  ComPrivateDataEntry& operator=(ComPrivateDataEntry&& other) noexcept;

  REFGUID guid() const { return m_guid; }
  bool hasGuid(REFGUID guid) const;
  HRESULT get(UINT& size, void* data) const;

 private:
  GUID m_guid{};
  ComPrivateDataType m_type = ComPrivateDataType::None;
  UINT m_size = 0;
  void* m_data = nullptr;
  IUnknown* m_iface = nullptr;

  void destroy();
};

class ComPrivateData {
 public:
  HRESULT setData(REFGUID guid, UINT size, const void* data);
  HRESULT setInterface(REFGUID guid, const IUnknown* iface);
  HRESULT getData(REFGUID guid, UINT* size, void* data);
  HRESULT removeData(REFGUID guid);

 private:
  std::vector<ComPrivateDataEntry> m_entries;

  ComPrivateDataEntry* findEntry(REFGUID guid);
  void insertEntry(ComPrivateDataEntry&& entry);
};

}  // namespace dxmt9::util
