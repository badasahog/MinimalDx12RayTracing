Simple DirectX 12 ray tracer written in C.

Based on the official Microsoft ray tracing sample:
https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingProceduralGeometry

This is also where I got the RT shader from, although I can't seem to compile it correctly.

In addition, this code needs to be compiled with clang or gcc. Compiling with the Microsoft Compiler causes an internal compiler error (not sure why).

The code has been heavily cleaned and rewritten from the initial sample, allowing for lower memory usage and `slightly` improved overall performance, as well as support for vsync.
<img width="1919" height="1027" alt="image" src="https://github.com/user-attachments/assets/5b621c6d-9c67-4934-93fb-4c063fc854d3" />
