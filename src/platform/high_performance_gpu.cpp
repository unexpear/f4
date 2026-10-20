// Hybrid-graphics laptops (NVIDIA Optimus, AMD PowerXpress / Switchable
// Graphics) run OpenGL on the integrated GPU unless the *executable* exports
// these symbols, which the vendor drivers read when their OpenGL ICD loads.
// The exports must live in the .exe itself, so CMake adds this file to every
// f4 executable (f4_prefer_high_performance_gpu). Exporting them makes the
// strongest GPU the OpenGL default; platform::Window clears them before the
// first GL context when F4_GPU=integrated asks for the integrated GPU.
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
