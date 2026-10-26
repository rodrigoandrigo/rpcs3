#version 450
layout(vertices = 3) out;
layout(location = 0) in float value_in[];
layout(location = 0) out float value_out[];
void main()
{
   gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
   gl_out[gl_InvocationID].gl_PointSize = gl_in[gl_InvocationID].gl_PointSize;
   barrier();
   value_out[gl_InvocationID] = value_in[gl_InvocationID] + gl_out[gl_InvocationID].gl_PointSize;
   if (gl_InvocationID == 0) {
      gl_TessLevelOuter[0] = 2;
      gl_TessLevelOuter[1] = 2;
      gl_TessLevelOuter[2] = 2;
      gl_TessLevelOuter[3] = 2;
      gl_TessLevelInner[0] = 2;
      gl_TessLevelInner[1] = 2;
   }
}
