# RPi-VK-Driver
RPi-VK-Driver is a low level GPU driver for the Broadcom Videocore IV GPU that implements a subset of the Vulkan (registered trademark of The Khronos Group) standard. The implementation is not conformant to the standard (therefore it cannot be called a Vulkan driver, officially) but tries to follow it as closely as the hardware allows for it.<br>
Compared to the available OpenGL drivers it offers superb speed including precise and predictable memory management and multi-threaded command submission. It also offers a wider feature set such as MSAA support, low level assembly shaders and performance counters.
On the other hand it currently does not support GLSL shaders.
