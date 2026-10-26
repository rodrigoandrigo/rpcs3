#version 450
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) in float value_in[];
layout(location = 0) out float value_out;
void main()
{
   gl_Position = gl_in[0].gl_Position;
   gl_PointSize = gl_in[0].gl_PointSize;
   value_out = value_in[0] + gl_PointSize;
   EmitVertex();
   EndPrimitive();
}
