#version 450
layout(location = 0) out float value;
void main()
{
   const vec2 positions[3] = vec2[3](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0, 0.5));
   gl_Position = vec4(positions[gl_VertexIndex % 3], 0.5, 1);
   gl_PointSize = 1.0;
   value = gl_PointSize;
}
