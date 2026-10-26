#version 450
#ifndef DOMAIN
#define DOMAIN triangles
#endif
#ifndef SPACING
#define SPACING equal_spacing
#endif
#ifdef POINT_MODE
layout(DOMAIN, SPACING, ccw, point_mode) in;
#else
layout(DOMAIN, SPACING, ccw) in;
#endif
layout(location = 0) in float value_in[];
layout(location = 0) out float value_out;
void main()
{
   gl_Position = gl_in[0].gl_Position * gl_TessCoord.x +
                 gl_in[1].gl_Position * gl_TessCoord.y +
                 gl_in[2].gl_Position * (1.0 - gl_TessCoord.x - gl_TessCoord.y);
   gl_PointSize = gl_in[0].gl_PointSize;
   value_out = value_in[0] + gl_PointSize;
}
