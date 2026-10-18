# dxmt9 dependencies
#
# Install system/bootstrap dependencies:
#   brew bundle

# Build system
brew "meson"
brew "ninja"
brew "ripgrep"
brew "mise"

# C/C++/ObjC compiler with full C++20 support.
# Also provides clang-tidy under the Homebrew LLVM prefix.
brew "llvm"
brew "clang-format"

# Formal verification — TLA+ model checker (TLC)
# verify_tla.sh falls back to `java -cp tla2tools.jar tlc2.TLC`.
cask "temurin"
# tla2tools.jar lives at:
#   /Applications/TLA+ Toolbox.app/Contents/Eclipse/tla2tools.jar
cask "tla+-toolbox"

# Optional experiment helpers
brew "msitools"
brew "winetricks"

# Offline cross-run analytics (experiments/analysis/).
# Installed by brew bundle; not an engine or capture runtime dependency.
brew "duckdb"

# Target application analysis with REA (see agents/rules/debug_rea.rules.md)
brew "ghidra"
