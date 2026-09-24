"""Generate a test-only equal-capacity linear interning counterfactual."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
start = source.index('  bool appendAccesses(const core::ExactResourceSet& set) noexcept {')
end = source.index('  std::unique_ptr<PreparedEncodePlanStorage> storage_{};', start)
assert source[start:end].count('lookup_[bucket]') == 1
linear = '''  bool appendAccesses(const core::ExactResourceSet& set) noexcept {
    for (std::uint32_t i = 0; i < set.count; ++i) {
      auto& s = *storage_;
      std::uint32_t index = 0;
      while (index < s.identityCount && s.identities[index] != set.handles[i]) ++index;
      if (s.accessCount == limits_.accesses ||
          (index == s.identityCount && index == limits_.identities))
        return fail(PreparedPlanFailure::Capacity);
      if (index == s.identityCount) s.identities[s.identityCount++] = set.handles[i];
      s.accesses[s.accessCount++] = static_cast<std::uint16_t>(index);
    }
    return true;
  }
'''
source = source[:start] + linear + source[end:]
scratch = '  std::array<std::uint16_t, kPreparedLookupCapacity> lookup_{};'
assert source.count(scratch) == 1
source = source.replace(scratch, '')
namespace = 'namespace dxmt9::encoders {'
assert source.count(namespace) == 1
source = source.replace(namespace, 'namespace dxmt9::linear_probe {\nusing namespace dxmt9::encoders;')
pathlib.Path(sys.argv[2]).write_text('// Generated test-only reference; never link into runtime.\n' + source)
