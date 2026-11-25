// Dip to Color: the built-in transition of the same name, as a plugin (template for a transition).
//
// transition(uv) returns the premultiplied RGBA of the output pixel. The convention is the one
// of gl-transitions (https://gl-transitions.com, MIT): uv has its origin at the BOTTOM left,
// getFromColor(uv) is the outgoing picture, getToColor(uv) the incoming one, `progress` runs
// from 0 (all outgoing) to 1 (all incoming) and `ratio` is width / height.
//
// u_color is declared for you from plugin.json (a colour parameter is a vec4, straight RGBA).
vec4 transition(vec2 uv)
{
    // The colour as premultiplied RGBA, like the pictures.
    vec4 c = vec4(u_color.rgb * u_color.a, u_color.a);
    // First half: outgoing picture to the colour; second half: colour to incoming picture.
    return progress < 0.5 ? mix(getFromColor(uv), c, progress * 2.0)
                          : mix(c, getToColor(uv), progress * 2.0 - 1.0);
}
