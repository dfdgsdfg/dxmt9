#include "../../../src/dxmt9/dxmt9_pending_clear_fusion.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace pc = dxmt9::pending_clear;
namespace {
void check(bool value, const char* label) {
  if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
}

pc::Facts base(unsigned mask = pc::Depth) {
  pc::Facts f;
  f.clearSource = f.drawSource = {
      .owner=1, .generation=2, .sequence=3, .sourceIndex=0,
      .storageGeneration=4, .firstPage=0, .pageCount=1};
  f.clearIndex = 7; f.drawIndex = 8;
  f.fullRect = true;
  f.requestedAspects = mask;
  f.draw[0] = {.resource=11, .alias=111, .width=2, .height=1, .samples=1,
               .aspects=pc::Color, .resolved=true};
  f.draw[4] = {.resource=12, .alias=112, .width=2, .height=1, .samples=1,
               .aspects=pc::Depth | pc::Stencil, .resolved=true};
  f.clear = f.draw;
  return f;
}

void admission() {
  auto reject = [](auto change, pc::Decision why) {
    auto f = base(); change(f);
    check(pc::classify(f) == why, "qualified rejection reason");
  };
  for (unsigned mask=1; mask<8; ++mask) check(pc::classify(base(mask)) == pc::Decision::Admit, "aspect combinations");
  reject([](auto& f) { f.clearSource.owner=0; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.generation; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.sequence; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.owner; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.sourceIndex; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.storageGeneration; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.firstPage; }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawSource.pageCount; }, pc::Decision::Order);
  reject([](auto& f) { f.clearSource.storageGeneration=0; }, pc::Decision::Order);
  reject([](auto& f) { f.clearSource.pageCount=0; }, pc::Decision::Order);
  reject([](auto& f) { f.clearSource.sourceIndex=std::numeric_limits<std::uint32_t>::max(); }, pc::Decision::Order);
  reject([](auto& f) { ++f.drawIndex; }, pc::Decision::Order);
  reject([](auto& f) { f.clearIndex=std::numeric_limits<std::uint32_t>::max(); f.drawIndex=0; }, pc::Decision::Order);
  reject([](auto& f) { f.activeEncoder=true; }, pc::Decision::Scope);
  reject([](auto& f) { f.queryScope=true; }, pc::Decision::Scope);
  reject([](auto& f) { f.requestedAspects=0; }, pc::Decision::EmptyOrUnknownAspects);
  reject([](auto& f) { f.requestedAspects=8; }, pc::Decision::EmptyOrUnknownAspects);
  reject([](auto& f) { f.fullRect=false; }, pc::Decision::PartialRect);
  reject([](auto& f) { f.srgbWrite=true; }, pc::Decision::ConversionOrDiscard);
  reject([](auto& f) { f.backbufferDiscardPending=true; }, pc::Decision::ConversionOrDiscard);
  reject([](auto& f) { f.exactReadHazard=true; }, pc::Decision::ReadHazard);
  for (std::size_t slot = 0; slot < base().sampledTextures.size(); ++slot) {
    reject([slot](auto& f) { f.sampledTextures[slot]=f.clear[4].alias; },
           pc::Decision::ReadHazard);
  }
  reject([](auto& f) { f.draw[4].resolved=false; }, pc::Decision::Unresolved);
  reject([](auto& f) { f.clear[4].resolved=false; }, pc::Decision::Unresolved);
  reject([](auto& f) { f.draw[4].resource=0; }, pc::Decision::Unresolved);
  reject([](auto& f) { ++f.clear[4].resource; }, pc::Decision::Identity);
  reject([](auto& f) { ++f.clear[4].boundLevel; }, pc::Decision::Identity);
  reject([](auto& f) { ++f.clear[4].viewLevel; }, pc::Decision::Identity);
  reject([](auto& f) { ++f.clear[4].slice; }, pc::Decision::Identity);
  reject([](auto& f) { ++f.clear[4].alias; }, pc::Decision::Identity);
  reject([](auto& f) { f.clear[4].width=1; }, pc::Decision::Extent);
  reject([](auto& f) { f.draw[0].width=1; }, pc::Decision::Extent);
  reject([](auto& f) { f.draw[0].height=2; }, pc::Decision::Extent);
  reject([](auto& f) { f.draw[0].width=0; }, pc::Decision::Extent);
  reject([](auto& f) { f.clear[4].samples=2; }, pc::Decision::SamplesOrResolve);
  reject([](auto& f) { f.draw[0].samples=2; }, pc::Decision::SamplesOrResolve);
  reject([](auto& f) { f.draw[0].hasResolveTarget=true; }, pc::Decision::SamplesOrResolve);
  reject([](auto& f) { f.clear[4].hasResolveTarget=true; }, pc::Decision::SamplesOrResolve);
  reject([](auto& f) { f.clear[4].aspects=pc::Stencil; }, pc::Decision::AspectMismatch);
  reject([](auto& f) { f.draw[4].aspects=pc::Stencil; }, pc::Decision::AspectMismatch);
  reject([](auto& f) { f.sampledTextures[15]=112; }, pc::Decision::ReadHazard);
  auto color = base(pc::Color);
  color.followingColorLoads[0] = pc::Load::Discard;
  check(pc::classify(color) == pc::Decision::FollowingLoadDiscardsClear, "first-use clear loss is not exact fusion");
  auto depth = base();
  depth.followingColorLoads[0] = pc::Load::Discard;
  check(pc::classify(depth) == pc::Decision::Admit, "unrequested discard remains unchanged");
  for (unsigned mask=0; mask<(1u<<14); ++mask) {
    auto f=base(pc::Color | pc::Depth);
    if (mask&1) ++f.drawSource.generation;
    if (mask&2) ++f.drawIndex;
    if (mask&4) f.activeEncoder=true;
    if (mask&8) f.queryScope=true;
    if (mask&16) f.fullRect=false;
    if (mask&32) f.srgbWrite=true;
    if (mask&64) f.backbufferDiscardPending=true;
    if (mask&128) f.draw[4].resolved=false;
    if (mask&256) ++f.clear[4].resource;
    if (mask&512) --f.draw[0].width;
    if (mask&1024) f.draw[0].samples=2;
    if (mask&2048) f.clear[4].aspects=pc::Stencil;
    if (mask&4096) f.sampledTextures[15]=112;
    if (mask&8192) f.followingColorLoads[0]=pc::Load::Discard;
    check((pc::classify(f) == pc::Decision::Admit) == (mask==0), "complete bounded guard truth table");
  }
  std::cout << "admission_truth_rows=16384\n";
}

using Memory = std::array<std::array<int,2>,3>;
Memory initial(unsigned bits) {
  Memory m{};
  for (unsigned a=0; a<3; ++a) for (unsigned x=0; x<2; ++x) m[a][x]=(bits>>(2*a+x))&1;
  return m;
}
void clearMemory(Memory& m, unsigned mask, unsigned values, unsigned width=2) {
  for (unsigned a=0; a<3; ++a) if (mask&(1u<<a))
    for (unsigned x=0; x<width; ++x) m[a][x]=(values>>a)&1;
}
void drawMemory(Memory& m, unsigned coverage, unsigned writes) {
  for (unsigned x=0; x<2; ++x) if ((coverage>>x)&1) {
    const auto before=m;
    for (unsigned a=0; a<3; ++a) if ((writes>>a)&1)
      m[a][x]=(before[0][x]^before[1][x]^before[2][x])&1;
  }
}

void refinement() {
  std::uint64_t states=0;
  for (unsigned mask=1; mask<8; ++mask) for (unsigned discardColor=0; discardColor<2; ++discardColor) {
    auto facts=base(mask);
    if (discardColor) facts.followingColorLoads[0]=pc::Load::Discard;
    if ((mask&pc::Color) && discardColor) {
      check(pc::classify(facts) == pc::Decision::FollowingLoadDiscardsClear, "serial clear discarded");
      continue;
    }
    check(pc::classify(facts) == pc::Decision::Admit, "shared admission");
    // Couple each bounded nondeterministic DontCare outcome independently per
    // pixel. Do not collapse all unknown pixels into one fixed sentinel value.
    for (unsigned loadChoice=0; loadChoice<(discardColor ? 4u : 1u); ++loadChoice)
    for (unsigned bits=0; bits<64; ++bits) for (unsigned values=0; values<8; ++values)
      for (unsigned coverage=0; coverage<4; ++coverage) for (unsigned writes=0; writes<8; ++writes) {
        auto reference=initial(bits);
        // Serial Clear encoder, Store, then Load into the following Draw.
        clearMemory(reference, mask, values);
        if (discardColor) reference[0]={int(loadChoice&1u), int((loadChoice>>1)&1u)};
        auto candidate=initial(bits);
        // Independent fused load-action selection; preserve other aspects.
        for (unsigned a=0; a<3; ++a) for (unsigned x=0; x<2; ++x)
          candidate[a][x] = mask&(1u<<a) ? (values>>a)&1 : a==0 && discardColor ? int((loadChoice>>x)&1u) : candidate[a][x];
        check(reference == candidate, "pre-draw contents and independent aspects");
        drawMemory(reference, coverage, writes); drawMemory(candidate, coverage, writes);
        check(reference == candidate, "ordered draw observation");
        for (unsigned discard=0; discard<8; ++discard) {
          auto r=reference, c=candidate;
          for (unsigned a=0; a<3; ++a) if (discard&(1u<<a)) r[a]=c[a]={-1,-1};
          check(r == c, "same final store policy");
        }
        ++states;
      }
  }
  // Distinguishing mutations: removing the extent guard under-clears pixel 1;
  // clearing the combined DS attachment corrupts the unrequested aspect.
  auto r=initial(0), c=r;
  clearMemory(r, pc::Depth, pc::Depth); clearMemory(c, pc::Depth, pc::Depth, 1);
  check(r != c, "extent counterexample");
  c=initial(0); clearMemory(c, pc::Depth|pc::Stencil, pc::Depth|pc::Stencil);
  check(r != c, "independent stencil counterexample");
  r=initial(0); clearMemory(r, pc::Color, pc::Color); c=r;
  r[0]={-1,-1}; check(r != c, "following DontCare is not exact preservation");
  std::cout << "refinement_states=" << states << " final_store_checks=" << states*8 << '\n';
}
} // namespace

int main() { admission(); refinement(); std::cout << "pending-clear fusion value model passed; runtime/GPU binding unproven\n"; }
