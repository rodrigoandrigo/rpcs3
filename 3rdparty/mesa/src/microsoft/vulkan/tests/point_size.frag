#version 450
layout(location = 0) in float value_in;
layout(location = 0) out vec4 color;
void main()
{
   color = vec4(value_in / 4.0, 0, 0, 1);
}
