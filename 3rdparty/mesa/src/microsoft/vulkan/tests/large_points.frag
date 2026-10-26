#version 450
layout(location = 0) out vec4 color;
void main()
{
   color = vec4(gl_PointCoord, 0.25, 1.0);
}
