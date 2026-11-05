#include <c3d/maths.h>

// Compat layer: render targets are landscape here, so the 3DS screen tilt (a quarter turn baked into the matrix)
// is left out. citro3d's "fovx / invaspect" naming of the tilted version describes the same frustum as
// Mtx_Persp(fovy, aspect): the vertical field of view across the 240-pixel side, aspect 400/240.
void Mtx_PerspTilt(C3D_Mtx* mtx, float fovx, float invaspect, float near, float far, bool isLeftHanded)
{
	Mtx_Persp(mtx, fovx, invaspect, near, far, isLeftHanded);
}
