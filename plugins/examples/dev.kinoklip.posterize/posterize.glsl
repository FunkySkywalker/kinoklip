// Posterize: the built-in effect of the same name, as a plugin (template for entry "main").
//
// entry "main": effect(uv) returns the premultiplied RGBA of the output pixel at texture
// coordinate uv (0,0 is the top left corner). texture(uTex, uv) is the clip's picture, also
// premultiplied. straight() turns a premultiplied colour into a straight one (helper provided).
//
// u_levels is declared for you from plugin.json (an integer parameter is a GLSL int).
vec4 effect(vec2 uv)
{
    vec4 t = texture(uTex, uv);
    if (t.a <= 0.0)
        return vec4(0.0); // keep transparent pixels transparent
    float n = float(u_levels); // ES does not convert int to float by itself
    vec3 c = clamp(straight(t), 0.0, 1.0);
    c = min(floor(c * n), n - 1.0) / (n - 1.0);
    return vec4(clamp(c, 0.0, 1.0) * t.a, t.a);
}
