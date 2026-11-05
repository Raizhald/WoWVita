#include <c3d/maths.h>

// Compat layer: no screen tilt (see mtx_persptilt.c).
void Mtx_OrthoTilt(C3D_Mtx* mtx, float left, float right, float bottom, float top, float near, float far, bool isLeftHanded)
{
	Mtx_Ortho(mtx, left, right, bottom, top, near, far, isLeftHanded);
}
