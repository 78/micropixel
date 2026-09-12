#ifndef MICROPIXEL_APPS_MAZE_BREAK_MAZE_BREAK_APP_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_MAZE_BREAK_APP_HPP

namespace maze_break {

// Entry point shared by main.cpp. Launch flags:
//   --benchmark   fixed-step autopilot; logs maze-break-bench lines every 120 frames
//   --no-bgm      skip the Opus background loop (isolates render cost)
//   --motion      opt into IMU movement/aiming; default is touch-only
//   --no-motion   force touch-only controls
//   --perf        show the FPS/RENDER/PRESENT/WAIT overlay (implied by --benchmark)
//   --mute        no audio at all (--benchmark implies it; --sound restores)
int MazeBreakAppMain();

}  // namespace maze_break

#endif
