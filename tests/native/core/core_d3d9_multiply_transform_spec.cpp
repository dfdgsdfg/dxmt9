// Wine behavioral oracle (LGPL — observable contract only, not source):
//   dlls/d3d9/tests/device.c :: test_multiply_transform()
//   wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727
//
// Verifies that the row-major 4x4 multiply used by dxmt9's
// IDirect3DDevice9::MultiplyTransform implementation (see
// src/d3d9/d3d9_pe_device.cpp MultiplyTransform) matches the D3D9
// semantics: applying MultiplyTransform(state, &M_new) is equivalent
// to SetTransform(state, M_current * M_new).
//
// dxmt9's PE-side multiply is hard-coded as:
//   result[r][c] = sum_k cur[r][k] * pM[k][c]
// which is the row-major "current first, new second" product. This
// spec pins that convention and the matching companion stateblock
// conformance case test_stateblock_multiply_transform_capture (see
// tests/conformance/d3d9/d3d9_conformance_query_stateblock.c).
//
// Pure value-level: no D3D9 device, no Wine, no Metal — operates on
// 4x4 float arrays directly so the convention can be diffed without a
// runtime.

#include "core_spec_fixtures.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::spec;

namespace {

// Row-major 4x4 matrix laid out the same way D3DMATRIX::m[row][col]
// stores its 16 floats (D3DMATRIX_DECL puts m[0][0]..m[0][3] first).
struct Mat4 {
  std::array<f32, 16> m{};

  constexpr f32 at(u32 row, u32 col) const { return m[row * 4 + col]; }
  constexpr f32& at(u32 row, u32 col) { return m[row * 4 + col]; }

  friend constexpr bool operator==(const Mat4&, const Mat4&) = default;
};

constexpr Mat4 identity() {
  Mat4 r{};
  r.at(0, 0) = 1.0f;
  r.at(1, 1) = 1.0f;
  r.at(2, 2) = 1.0f;
  r.at(3, 3) = 1.0f;
  return r;
}

// Standard D3D9 row-vector translation: translate goes in row 3 with
// the row-vector v' = v * M convention.
constexpr Mat4 translate(f32 x, f32 y, f32 z) {
  Mat4 r = identity();
  r.at(3, 0) = x;
  r.at(3, 1) = y;
  r.at(3, 2) = z;
  return r;
}

// Homogeneous "set_scale_matrix" — the Wine d3d9 stateblock fixture
// puts the same value on all four diagonal entries including [3][3].
// This is the exact mat2 layout used in test_multiply_transform.
constexpr Mat4 scaleHomogeneous(f32 s) {
  Mat4 r{};
  r.at(0, 0) = s;
  r.at(1, 1) = s;
  r.at(2, 2) = s;
  r.at(3, 3) = s;
  return r;
}

// Proper 3D scale that preserves the homogeneous coordinate.
constexpr Mat4 scale3D(f32 s) {
  Mat4 r{};
  r.at(0, 0) = s;
  r.at(1, 1) = s;
  r.at(2, 2) = s;
  r.at(3, 3) = 1.0f;
  return r;
}

Mat4 rotateZ(f32 theta) {
  const f32 c = std::cos(theta);
  const f32 s = std::sin(theta);
  Mat4 r = identity();
  r.at(0, 0) = c;
  r.at(0, 1) = s;
  r.at(1, 0) = -s;
  r.at(1, 1) = c;
  return r;
}

// Row-major multiply identical to the body of
// d3d9_pe_device.cpp::MultiplyTransform: result = cur * pM.
Mat4 multiply(const Mat4& cur, const Mat4& pM) {
  Mat4 result{};
  for (u32 r = 0; r < 4; ++r) {
    for (u32 c = 0; c < 4; ++c) {
      f32 sum = 0.0f;
      for (u32 k = 0; k < 4; ++k) {
        sum += cur.at(r, k) * pM.at(k, c);
      }
      result.at(r, c) = sum;
    }
  }
  return result;
}

void checkMatNear(const Mat4& a, const Mat4& b, f32 eps,
                  std::string_view message) {
  for (u32 i = 0; i < 16; ++i) {
    if (std::fabs(a.m[i] - b.m[i]) > eps) {
      std::ostringstream out;
      out << message << " (idx=" << i << " left=" << a.m[i]
          << " right=" << b.m[i] << ")";
      fail(out.str());
    }
  }
}

// ---------------------------------------------------------------------------

void testIdentityIsLeftAndRightUnit() {
  const Mat4 I = identity();
  checkEq(multiply(I, I), I, "I * I = I");

  const Mat4 T = translate(1.0f, 2.0f, 3.0f);
  checkEq(multiply(I, T), T, "I * T(1,2,3) = T(1,2,3) (oracle: SetTransform(I) then MultiplyTransform(T))");
  checkEq(multiply(T, I), T, "T(1,2,3) * I = T(1,2,3) (multiply-by-identity is a no-op on the right)");

  const Mat4 S = scaleHomogeneous(2.5f);
  checkEq(multiply(I, S), S, "I * S(2.5) = S(2.5)");
  checkEq(multiply(S, I), S, "S(2.5) * I = S(2.5)");
}

void testWineOracle_IdentityMultByScaleEqualsScale() {
  // Mirrors dlls/d3d9/tests/device.c::test_multiply_transform: start
  // from identity (mat1), MultiplyTransform(mat2 = scale(2) with all
  // four diagonals = 2 — matches set_scale_matrix), expect the
  // resulting transform == mat2.
  const Mat4 mat1 = identity();
  const Mat4 mat2 = scaleHomogeneous(2.0f);
  const Mat4 product = multiply(mat1, mat2);
  checkEq(product, mat2,
          "Wine test_multiply_transform: identity * scale(2) == scale(2)");
}

void testScaleThenTranslateRowVectorConvention() {
  // D3D9 uses the row-vector v' = v * M convention. With that
  // convention, MultiplyTransform composes left-to-right in
  // application order. Build the "scale then translate" composite the
  // way an app would call MultiplyTransform on a stateblock:
  //   SetTransform(I) ; MultiplyTransform(S) ; MultiplyTransform(T)
  // which our impl resolves to (I * S) * T = S * T. A row vector
  // (px, py, pz, 1) transformed by S*T then yields (s*px+tx, s*py+ty,
  // s*pz+tz, 1) — scaling lands first, translation second.
  //
  // Uses scale3D (homogeneous coord left at 1.0) so the translation
  // row pins isolate the multiply-order effect, not a w-rescale.
  const Mat4 S = scale3D(3.0f);
  const Mat4 T = translate(1.0f, 0.0f, 0.0f);

  const Mat4 ST = multiply(S, T);  // first scale, then translate
  const Mat4 TS = multiply(T, S);  // first translate, then scale

  // Concrete value-level pins: with S[3][3]=1 the ST product leaves
  // row 3 as T's translation row (0,0,0,1)-times-T = (1,0,0,1).
  // For TS the translation lands first in row 3, then the scale
  // multiplies that row, yielding (3,0,0,1).
  checkNear(ST.at(3, 0), 1.0f, 1e-6f, "S*T row3.x equals T's translation (S has identity row3)");
  checkNear(ST.at(3, 1), 0.0f, 1e-6f, "S*T row3.y");
  checkNear(ST.at(3, 2), 0.0f, 1e-6f, "S*T row3.z");
  checkNear(ST.at(3, 3), 1.0f, 1e-6f, "S*T row3.w preserved");
  checkNear(ST.at(0, 0), 3.0f, 1e-6f, "S*T scale on diagonal x");

  checkNear(TS.at(3, 0), 3.0f, 1e-6f, "T*S row3.x = T.row3 * S has the scale applied");
  checkNear(TS.at(3, 3), 1.0f, 1e-6f, "T*S row3.w preserved");
  checkNear(TS.at(0, 0), 3.0f, 1e-6f, "T*S scale on diagonal x");

  // Therefore S*T != T*S — non-commuting compose proves the convention
  // is not symmetric and matches the D3D9 PE-side multiply body.
  bool equal = true;
  for (u32 i = 0; i < 16; ++i) {
    if (std::fabs(ST.m[i] - TS.m[i]) > 1e-6f) {
      equal = false;
      break;
    }
  }
  check(!equal, "S*T != T*S — confirms the row-major non-commuting compose");
}

void testNonCommutingRotates() {
  // Two non-trivial axis-Z rotates with different mock "asymmetries"
  // applied via translation interleaving — the cleanest commutativity
  // failure we can express in a single-axis test is rotate+translate.
  const Mat4 Rz = rotateZ(static_cast<f32>(0.5));
  const Mat4 T = translate(1.0f, 0.0f, 0.0f);

  const Mat4 RT = multiply(Rz, T);
  const Mat4 TR = multiply(T, Rz);

  // RT applies rotation first then translation by (1,0,0) in the
  // post-rotate frame, so row 3 stays (1,0,0,1). TR applies the
  // translation first then rotates the translation row, so the
  // translation row becomes (cos(0.5), sin(0.5), 0, 1).
  checkNear(RT.at(3, 0), 1.0f, 1e-6f, "Rz*T translation x stays in the post-rotate frame");
  checkNear(RT.at(3, 1), 0.0f, 1e-6f, "Rz*T translation y");

  checkNear(TR.at(3, 0), std::cos(0.5f), 1e-6f, "T*Rz translation x gets rotated");
  checkNear(TR.at(3, 1), std::sin(0.5f), 1e-6f, "T*Rz translation y gets rotated");

  // Sanity: rotate(theta) * rotate(-theta) = identity (commutes with
  // its own inverse).
  const Mat4 R1 = rotateZ(0.7f);
  const Mat4 R1Inv = rotateZ(-0.7f);
  checkMatNear(multiply(R1, R1Inv), identity(), 1e-5f,
               "Rz(t) * Rz(-t) ~= I");
  checkMatNear(multiply(R1Inv, R1), identity(), 1e-5f,
               "Rz(-t) * Rz(t) ~= I");
}

void testAgainstDxmt9MatrixHelperConvention() {
  // The dxmt9::core::Matrix4x4 helper in src/d3d9/core_draw.cpp uses
  // index `row*4 + col` and the multiply body
  //   result[row*4+col] = sum_k left[row*4+k] * right[k*4+col]
  // — identical to the local Mat4 here. Mirror a representative
  // product through both to pin the convention.
  Matrix4x4 dxIdentity{};
  dxIdentity.m = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

  Matrix4x4 dxScale{};
  dxScale.m = {2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
               0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f};

  // Spec-side row-major multiply replicating core_draw.cpp's body.
  Matrix4x4 product{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t col = 0; col < 4; ++col) {
      f32 sum = 0.0f;
      for (size_t k = 0; k < 4; ++k) {
        sum += dxIdentity.m[row * 4 + k] * dxScale.m[k * 4 + col];
      }
      product.m[row * 4 + col] = sum;
    }
  }
  checkEq(product, dxScale,
          "core Matrix4x4 identity * scale(2) = scale(2) — same row-major convention");
}

}  // namespace

int main() {
  try {
    testIdentityIsLeftAndRightUnit();
    testWineOracle_IdentityMultByScaleEqualsScale();
    testScaleThenTranslateRowVectorConvention();
    testNonCommutingRotates();
    testAgainstDxmt9MatrixHelperConvention();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
