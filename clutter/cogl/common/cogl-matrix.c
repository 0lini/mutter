#include <cogl-matrix.h>

#include <glib.h>
#include <math.h>
#include <string.h>

void
cogl_matrix_init_identity (CoglMatrix *matrix)
{
  matrix->xx = 1; matrix->xy = 0; matrix->xz = 0; matrix->xw = 0;
  matrix->yx = 0; matrix->yy = 1; matrix->yz = 0; matrix->yw = 0;
  matrix->zx = 0; matrix->zy = 0; matrix->zz = 1; matrix->zw = 0;
  matrix->wx = 0; matrix->wy = 0; matrix->wz = 0; matrix->ww = 1;
}

void
cogl_matrix_multiply (CoglMatrix *result,
		      const CoglMatrix *a,
		      const CoglMatrix *b)
{
  CoglMatrix r;

  /* row 0 */
  r.xx = a->xx * b->xx + a->xy * b->yx + a->xz * b->zx + a->xw * b->wx;
  r.xy = a->xx * b->xy + a->xy * b->yy + a->xz * b->zy + a->xw * b->wy;
  r.xz = a->xx * b->xz + a->xy * b->yz + a->xz * b->zz + a->xw * b->wz;
  r.xw = a->xx * b->xw + a->xy * b->yw + a->xz * b->zw + a->xw * b->ww;

  /* row 1 */
  r.yx = a->yx * b->xx + a->yy * b->yx + a->yz * b->zx + a->yw * b->wx;
  r.yy = a->yx * b->xy + a->yy * b->yy + a->yz * b->zy + a->yw * b->wy;
  r.yz = a->yx * b->xz + a->yy * b->yz + a->yz * b->zz + a->yw * b->wz;
  r.yw = a->yx * b->xw + a->yy * b->yw + a->yz * b->zw + a->yw * b->ww;

  /* row 2 */
  r.zx = a->zx * b->xx + a->zy * b->yx + a->zz * b->zx + a->zw * b->wx;
  r.zy = a->zx * b->xy + a->zy * b->yy + a->zz * b->zy + a->zw * b->wy;
  r.zz = a->zx * b->xz + a->zy * b->yz + a->zz * b->zz + a->zw * b->wz;
  r.zw = a->zx * b->xw + a->zy * b->yw + a->zz * b->zw + a->zw * b->ww;

  /* row 3 */
  r.wx = a->wx * b->xx + a->wy * b->yx + a->wz * b->zx + a->ww * b->wx;
  r.wy = a->wx * b->xy + a->wy * b->yy + a->wz * b->zy + a->ww * b->wy;
  r.wz = a->wx * b->xz + a->wy * b->yz + a->wz * b->zz + a->ww * b->wz;
  r.ww = a->wx * b->xw + a->wy * b->yw + a->wz * b->zw + a->ww * b->ww;

  /* The idea was that having this unrolled; it might be easier for the
   * compiler to vectorize, but that's probably not true. Mesa does it
   * using a single for (i=0; i<4; i++) approach, may that's better...
   */

  *result = r;
}

void
cogl_matrix_rotate (CoglMatrix *matrix,
		    float angle,
		    float x,
		    float y,
		    float z)
{
  CoglMatrix rotation;
  CoglMatrix result;
  angle *= G_PI / 180.0f;
  float c = cosf (angle);
  float s = sinf (angle);

  rotation.xx = x * x * (1.0f - c) + c;
  rotation.yx = y * x * (1.0f - c) + z * s;
  rotation.zx = x * z * (1.0f - c) - y * s;
  rotation.wx = 0.0f;

  rotation.xy = x * y * (1.0f - c) - z * s;
  rotation.yy = y * y * (1.0f - c) + c;
  rotation.zy = y * z * (1.0f - c) + x * s;
  rotation.wy = 0.0f;

  rotation.xz = x * z * (1.0f - c) + y * s;
  rotation.yz = y * z * (1.0f - c) - x * s;
  rotation.zz = z * z * (1.0f - c) + c;
  rotation.wz = 0.0f;

  rotation.xw = 0.0f;
  rotation.yw = 0.0f;
  rotation.zw = 0.0f;
  rotation.ww = 1.0f;

  cogl_matrix_multiply (&result, matrix, &rotation);
  *matrix = result;
}

void
cogl_matrix_translate (CoglMatrix *matrix,
		       float x,
		       float y,
		       float z)
{
  matrix->xw = matrix->xx * x + matrix->xy * y + matrix->xz * z + matrix->xw;
  matrix->yw = matrix->yx * x + matrix->yy * y + matrix->yz * z + matrix->yw;
  matrix->zw = matrix->zx * x + matrix->zy * y + matrix->zz * z + matrix->zw;
  matrix->ww = matrix->wx * x + matrix->wy * y + matrix->wz * z + matrix->ww;
}

void
cogl_matrix_scale (CoglMatrix *matrix,
		   float sx,
		   float sy,
		   float sz)
{
  matrix->xx *= sx; matrix->xy *= sy; matrix->xz *= sz;
  matrix->yx *= sx; matrix->yy *= sy; matrix->yz *= sz;
  matrix->zx *= sx; matrix->zy *= sy; matrix->zz *= sz;
  matrix->wx *= sx; matrix->wy *= sy; matrix->wz *= sz;
}

#if 0
gboolean
cogl_matrix_invert (CoglMatrix *matrix)
{
  /* TODO */
  /* Note: It might be nice to also use the flag based tricks that mesa does
   * to alow it to track the type of transformations a matrix represents
   * so it can use various assumptions to optimise the inversion.
   */
}
#endif

void
cogl_matrix_transform_point (const CoglMatrix *matrix,
                             float *x,
                             float *y,
                             float *z,
                             float *w)
{
  float _x = *x, _y = *y, _z = *z, _w = *w;

  *x = matrix->xx * _x + matrix->xy * _y + matrix->xz * _z + matrix->xw * _w;
  *y = matrix->yx * _x + matrix->yy * _y + matrix->yz * _z + matrix->yw * _w;
  *z = matrix->zx * _x + matrix->zy * _y + matrix->zz * _z + matrix->zw * _w;
  *w = matrix->wx * _x + matrix->wy * _y + matrix->wz * _z + matrix->ww * _w;
}

void
cogl_matrix_init_from_array (CoglMatrix *matrix, const float *array)
{
  memcpy (matrix, array, sizeof (float) * 16);
}

const float *
cogl_matrix_get_array (const CoglMatrix *matrix)
{
  return (float *)matrix;
}

