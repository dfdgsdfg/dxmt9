// Reuse the catalogue's licensed, self-checking triangle fixture. The bounded
// post-work dwell lets an asynchronous tracing window close before Wine exits.
// This fixture qualifies capture/ABI correctness, not application throughput.
#define WinMain dxmt9_tracing_triangle_fixture_main
#include "../../../experiments/apps/conf-d3d9-triangle/conf_d3d9_triangle.cpp"
#undef WinMain

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine,
                   int showCommand) {
  const int result = dxmt9_tracing_triangle_fixture_main(
      instance, previous, commandLine, showCommand);
  Sleep(1500);
  return result;
}
