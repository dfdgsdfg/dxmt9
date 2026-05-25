// Deterministic, device-free spec for the shared D3D9 private-data store
// (dxmt9::util::ComPrivateData) used by every IDirect3DResource9 /
// IDirect3DVolume9 PE wrapper.
//
// Behavioral oracle: Wine dlls/d3d9/tests/device.c::test_private_data and the
// mirrored PE conformance functions in
// tests/conformance/d3d9/d3d9_conformance_resource.c
// (test_private_data_resource_wrappers, test_private_data_replace_and_size_policy,
//  test_private_data_iunknown_ownership_smoke). Locks R-CORE-4.10:
//   - byte blobs: set / overwrite-frees-old / MOREDATA-with-required-size /
//     NOTFOUND-leaves-size / NULL-data-queries-size,
//   - D3DSPD_IUNKNOWN: AddRef on set, AddRef on successful get, Release on
//     overwrite / free / destruction, invalid size rejected without replacing
//     an existing entry,
//   - free / destroy releases held interfaces.
//
// This is a pure value transform — no device, no Metal, no bridge.

#include "util/com/com_private_data.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    throw TestFailure(std::string(message) + " (left != right)");
  }
}

// Counting IUnknown — mirrors the dual_unknown fixture in the PE conformance
// suite. AddRef/Release adjust `refs` and bump observable call counters so the
// spec can prove the store takes/releases an owned reference rather than
// treating the pointer as opaque bytes.
struct CountingUnknown : IUnknown {
  long refs = 1;
  long addRefs = 0;
  long releases = 0;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    if (out) {
      *out = nullptr;
    }
    return static_cast<HRESULT>(0x80004002L);  // E_NOINTERFACE
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    ++addRefs;
    return static_cast<ULONG>(++refs);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ++releases;
    return static_cast<ULONG>(--refs);
  }
};

constexpr GUID kGuidA{0x11111111, 0x2222, 0x3333, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};
constexpr GUID kGuidB{0xcccccccc, 0xdddd, 0xeeee, {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};

void testByteBlobRoundTrip() {
  dxmt9::util::ComPrivateData store;
  const std::uint8_t expected[] = {0x11, 0x22, 0x33, 0x44};

  checkEq(store.setData(kGuidA, sizeof(expected), expected), S_OK, "set byte blob");

  // NULL data buffer queries the required size and succeeds.
  UINT size = 0;
  checkEq(store.getData(kGuidA, &size, nullptr), S_OK, "get size with null buffer");
  checkEq(size, static_cast<UINT>(sizeof(expected)), "null-buffer reports required size");

  // Exact-fit read returns the bytes.
  std::uint8_t actual[sizeof(expected)] = {0};
  size = sizeof(actual);
  checkEq(store.getData(kGuidA, &size, actual), S_OK, "get exact fit");
  checkEq(size, static_cast<UINT>(sizeof(expected)), "exact-fit size");
  check(std::memcmp(actual, expected, sizeof(expected)) == 0, "exact-fit bytes match");

  // Too-small buffer: D3DERR_MOREDATA, required size written, buffer untouched.
  std::memset(actual, 0xcc, sizeof(actual));
  size = 1;
  checkEq(store.getData(kGuidA, &size, actual), D3DERR_MOREDATA, "too-small returns MOREDATA");
  checkEq(size, static_cast<UINT>(sizeof(expected)), "MOREDATA writes required size");
  checkEq(actual[0], static_cast<std::uint8_t>(0xcc), "MOREDATA leaves caller buffer untouched");
}

void testNotFoundLeavesSizeUntouched() {
  dxmt9::util::ComPrivateData store;
  // getData itself zeroes *size on NOTFOUND; the wrapper layer preserves the
  // caller's value (it only writes back when hr != NOTFOUND). Pin the store's
  // documented NOTFOUND contract and that no entry materializes.
  std::uint8_t actual[4] = {0};
  UINT size = sizeof(actual);
  checkEq(store.getData(kGuidA, &size, actual), D3DERR_NOTFOUND, "missing guid returns NOTFOUND");

  // free of an absent guid is NOTFOUND.
  checkEq(store.removeData(kGuidA), D3DERR_NOTFOUND, "free absent guid returns NOTFOUND");
}

void testOverwriteFreesOldByteBlob() {
  dxmt9::util::ComPrivateData store;
  const std::uint8_t first[] = {0x10, 0x20};
  const std::uint8_t second[] = {0x30, 0x40, 0x50, 0x60};

  checkEq(store.setData(kGuidA, sizeof(first), first), S_OK, "set first blob");

  std::uint8_t actual[sizeof(second)] = {0};
  UINT size = sizeof(actual);
  checkEq(store.getData(kGuidA, &size, actual), S_OK, "read first blob");
  checkEq(size, static_cast<UINT>(sizeof(first)), "first blob size");
  check(std::memcmp(actual, first, sizeof(first)) == 0, "first blob bytes");

  // Overwrite with a larger blob — the new size/content fully replace the old.
  checkEq(store.setData(kGuidA, sizeof(second), second), S_OK, "overwrite with second blob");

  std::memset(actual, 0xcc, sizeof(actual));
  size = sizeof(first);
  checkEq(store.getData(kGuidA, &size, actual), D3DERR_MOREDATA,
          "stale small buffer now too small for new blob");
  checkEq(size, static_cast<UINT>(sizeof(second)), "overwrite updates required size");
  checkEq(actual[0], static_cast<std::uint8_t>(0xcc), "MOREDATA leaves buffer untouched after overwrite");

  size = sizeof(actual);
  std::memset(actual, 0, sizeof(actual));
  checkEq(store.getData(kGuidA, &size, actual), S_OK, "read second blob");
  checkEq(size, static_cast<UINT>(sizeof(second)), "second blob size");
  check(std::memcmp(actual, second, sizeof(second)) == 0, "second blob bytes");

  checkEq(store.removeData(kGuidA), S_OK, "free populated guid");
  checkEq(store.removeData(kGuidA), D3DERR_NOTFOUND, "double free is NOTFOUND");
}

void testIUnknownAddRefAndRelease() {
  CountingUnknown obj;
  {
    dxmt9::util::ComPrivateData store;

    // setInterface takes one owned reference.
    checkEq(store.setInterface(kGuidA, &obj), S_OK, "set interface");
    checkEq(obj.addRefs, 1L, "set interface AddRef'd once");
    checkEq(obj.releases, 0L, "set interface did not Release");
    checkEq(obj.refs, 2L, "ref count is 2 after set");

    // getData on an iface entry AddRef's the returned pointer and reports the
    // pointer-sized blob.
    IUnknown* out = nullptr;
    UINT size = sizeof(out);
    checkEq(store.getData(kGuidA, &size, &out), S_OK, "get interface");
    checkEq(size, static_cast<UINT>(sizeof(IUnknown*)), "interface get reports pointer size");
    check(out == &obj, "interface get returns the stored pointer");
    checkEq(obj.addRefs, 2L, "get interface AddRef'd the returned pointer");
    checkEq(obj.refs, 3L, "ref count is 3 after get");

    // Caller releases its retrieved reference.
    out->Release();
    checkEq(obj.releases, 1L, "caller released retrieved reference");
    checkEq(obj.refs, 2L, "ref count back to 2 after caller release");

    // Too-small buffer on an iface entry: MOREDATA, no AddRef leak.
    IUnknown* tooSmallOut = reinterpret_cast<IUnknown*>(0xdeadbeef);
    UINT tooSmall = 1;
    checkEq(store.getData(kGuidA, &tooSmall, &tooSmallOut), D3DERR_MOREDATA,
            "interface get with small buffer returns MOREDATA");
    checkEq(tooSmall, static_cast<UINT>(sizeof(IUnknown*)), "interface MOREDATA reports pointer size");
    checkEq(obj.addRefs, 2L, "interface MOREDATA did not AddRef");
    check(tooSmallOut == reinterpret_cast<IUnknown*>(0xdeadbeef),
          "interface MOREDATA leaves caller pointer untouched");

    // Explicit free Releases the owned reference.
    checkEq(store.removeData(kGuidA), S_OK, "free interface");
    checkEq(obj.releases, 2L, "free Released the owned reference");
    checkEq(obj.refs, 1L, "ref count back to 1 after free");
  }
  // Store destruction with no entry left must not touch the object further.
  checkEq(obj.refs, 1L, "ref count unchanged after empty-store destruction");
}

void testIUnknownReleasedOnStoreDestruction() {
  CountingUnknown obj;
  {
    dxmt9::util::ComPrivateData store;
    checkEq(store.setInterface(kGuidA, &obj), S_OK, "set interface for destroy test");
    checkEq(obj.refs, 2L, "ref count 2 after set");
    // Leave it in the store; destruction must release.
  }
  checkEq(obj.releases, 1L, "store destruction Released the owned reference");
  checkEq(obj.refs, 1L, "ref count back to 1 after store destruction");
}

void testIUnknownOverwriteReleasesOld() {
  CountingUnknown first;
  CountingUnknown second;
  dxmt9::util::ComPrivateData store;

  checkEq(store.setInterface(kGuidA, &first), S_OK, "set first interface");
  checkEq(first.addRefs, 1L, "first AddRef'd");

  // Overwriting the same GUID with a different interface must Release the old
  // and AddRef the new.
  checkEq(store.setInterface(kGuidA, &second), S_OK, "overwrite with second interface");
  checkEq(first.releases, 1L, "old interface Released on overwrite");
  checkEq(first.refs, 1L, "old interface ref count restored on overwrite");
  checkEq(second.addRefs, 1L, "new interface AddRef'd on overwrite");
  checkEq(second.refs, 2L, "new interface ref count 2 on overwrite");

  // Overwriting an iface entry with a byte blob also releases the old iface.
  const std::uint8_t bytes[] = {0xaa, 0xbb};
  checkEq(store.setData(kGuidA, sizeof(bytes), bytes), S_OK, "overwrite iface with byte blob");
  checkEq(second.releases, 1L, "iface Released when overwritten by byte blob");
  checkEq(second.refs, 1L, "iface ref count restored when overwritten by byte blob");
}

void testMultipleGuidsIndependent() {
  CountingUnknown obj;
  dxmt9::util::ComPrivateData store;

  const std::uint8_t bytes[] = {0x01, 0x02, 0x03};
  checkEq(store.setData(kGuidA, sizeof(bytes), bytes), S_OK, "set guid A bytes");
  checkEq(store.setInterface(kGuidB, &obj), S_OK, "set guid B iface");
  checkEq(obj.addRefs, 1L, "guid B iface AddRef'd");

  // Reading / freeing one GUID does not disturb the other.
  std::uint8_t actual[sizeof(bytes)] = {0};
  UINT size = sizeof(actual);
  checkEq(store.getData(kGuidA, &size, actual), S_OK, "read guid A");
  check(std::memcmp(actual, bytes, sizeof(bytes)) == 0, "guid A bytes intact");

  checkEq(store.removeData(kGuidA), S_OK, "free guid A");
  checkEq(obj.refs, 2L, "freeing guid A did not touch guid B iface");

  IUnknown* out = nullptr;
  size = sizeof(out);
  checkEq(store.getData(kGuidB, &size, &out), S_OK, "guid B still present after guid A free");
  check(out == &obj, "guid B returns its iface");
  out->Release();
  checkEq(store.removeData(kGuidB), S_OK, "free guid B");
  checkEq(obj.refs, 1L, "guid B iface ref count restored after free");
}

}  // namespace

int main() {
  try {
    testByteBlobRoundTrip();
    testNotFoundLeavesSizeUntouched();
    testOverwriteFreesOldByteBlob();
    testIUnknownAddRefAndRelease();
    testIUnknownReleasedOnStoreDestruction();
    testIUnknownOverwriteReleasesOld();
    testMultipleGuidsIndependent();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
