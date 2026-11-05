#include <c3d/maths.h>

// Compat layer: no screen tilt (see mtx_persptilt.c).
void Mtx_PerspStereoTilt(C3D_Mtx* mtx, float fovx, float invaspect, float near, float far, float iod, float screen, bool isLeftHanded)
{
	Mtx_PerspStereo(mtx, fovx, invaspect, near, far, iod, screen, isLeftHanded);
}
