#include "com_private_data.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt9::util {

ComPrivateDataEntry::ComPrivateDataEntry() {
  m_guid = GUID{};
}

ComPrivateDataEntry::ComPrivateDataEntry(REFGUID guid, UINT size, const void* data)
    : m_guid(guid), m_type(ComPrivateDataType::Data), m_size(size), m_data(std::malloc(size)) {
  std::memcpy(m_data, data, size);
}

ComPrivateDataEntry::ComPrivateDataEntry(REFGUID guid, const IUnknown* iface)
    : m_guid(guid), m_type(ComPrivateDataType::Iface), m_iface(const_cast<IUnknown*>(iface)) {
  if (m_iface) {
    m_iface->AddRef();
  }
}

ComPrivateDataEntry::~ComPrivateDataEntry() {
  destroy();
}

ComPrivateDataEntry::ComPrivateDataEntry(ComPrivateDataEntry&& other) noexcept
    : m_guid(other.m_guid), m_type(other.m_type), m_size(other.m_size), m_data(other.m_data), m_iface(other.m_iface) {
  other.m_guid = GUID{};
  other.m_type = ComPrivateDataType::None;
  other.m_size = 0;
  other.m_data = nullptr;
  other.m_iface = nullptr;
}

ComPrivateDataEntry& ComPrivateDataEntry::operator=(ComPrivateDataEntry&& other) noexcept {
  destroy();
  m_guid = other.m_guid;
  m_type = other.m_type;
  m_size = other.m_size;
  m_data = other.m_data;
  m_iface = other.m_iface;

  other.m_guid = GUID{};
  other.m_type = ComPrivateDataType::None;
  other.m_size = 0;
  other.m_data = nullptr;
  other.m_iface = nullptr;
  return *this;
}

bool ComPrivateDataEntry::hasGuid(REFGUID guid) const {
  return InlineIsEqualGUID(m_guid, guid) != FALSE;
}

HRESULT ComPrivateDataEntry::get(UINT& size, void* data) const {
  UINT requiredSize = 0;
  if (m_type == ComPrivateDataType::Iface) {
    requiredSize = sizeof(IUnknown*);
  } else if (m_type == ComPrivateDataType::Data) {
    requiredSize = m_size;
  }

  if (!data) {
    size = requiredSize;
    return S_OK;
  }

  const HRESULT result = size < requiredSize ? D3DERR_MOREDATA : S_OK;
  if (size >= requiredSize) {
    if (m_type == ComPrivateDataType::Iface) {
      if (m_iface) {
        m_iface->AddRef();
      }
      std::memcpy(data, &m_iface, requiredSize);
    } else {
      std::memcpy(data, m_data, requiredSize);
    }
  }

  size = requiredSize;
  return result;
}

void ComPrivateDataEntry::destroy() {
  if (m_data) {
    std::free(m_data);
  }
  if (m_iface) {
    m_iface->Release();
  }
  m_data = nullptr;
  m_iface = nullptr;
  m_size = 0;
  m_type = ComPrivateDataType::None;
}

HRESULT ComPrivateData::setData(REFGUID guid, UINT size, const void* data) {
  if (!data) {
    return removeData(guid);
  }
  insertEntry(ComPrivateDataEntry(guid, size, data));
  return S_OK;
}

HRESULT ComPrivateData::setInterface(REFGUID guid, const IUnknown* iface) {
  insertEntry(ComPrivateDataEntry(guid, iface));
  return S_OK;
}

HRESULT ComPrivateData::getData(REFGUID guid, UINT* size, void* data) {
  if (!size) {
    return D3DERR_INVALIDCALL;
  }

  auto* entry = findEntry(guid);
  if (!entry) {
    *size = 0;
    return D3DERR_NOTFOUND;
  }

  return entry->get(*size, data);
}

HRESULT ComPrivateData::removeData(REFGUID guid) {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    if (it->hasGuid(guid)) {
      m_entries.erase(it);
      return S_OK;
    }
  }
  return D3DERR_NOTFOUND;
}

ComPrivateDataEntry* ComPrivateData::findEntry(REFGUID guid) {
  for (auto& entry : m_entries) {
    if (entry.hasGuid(guid)) {
      return &entry;
    }
  }
  return nullptr;
}

void ComPrivateData::insertEntry(ComPrivateDataEntry&& entry) {
  ComPrivateDataEntry source = std::move(entry);
  auto* existing = findEntry(source.guid());
  if (existing) {
    *existing = std::move(source);
  } else {
    m_entries.push_back(std::move(source));
  }
}

}  // namespace dxmt9::util
