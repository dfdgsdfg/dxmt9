#include "com_private_data.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt9::util {

ComPrivateDataEntry::ComPrivateDataEntry(REFGUID guid, const IUnknown* iface)
    : m_guid(guid), m_type(ComPrivateDataType::Iface), m_iface(const_cast<IUnknown*>(iface)) {
  if (m_iface) {
    m_iface->AddRef();
  }
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

}  // namespace dxmt9::util
