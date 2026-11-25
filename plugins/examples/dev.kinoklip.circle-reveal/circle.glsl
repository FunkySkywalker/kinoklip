// Circle Reveal: the incoming picture opens as a circle from the middle.
//
// gl-transitions convention: uv has its origin at the bottom left; getFromColor(uv) is the
// outgoing picture, getToColor(uv) the incoming one, progress runs 0..1, ratio is width / height.
// u_softness (float) and u_invert (bool) are declared for you from plugin.json.
vec4 transition(vec2 uv)
{
    // Distance from the middle, with the x axis scaled so the circle is round.
    float d = length((uv - 0.5) * vec2(ratio, 1.0));
    // The circle must be wider than the corner of the picture at the end.
    float reach = 0.5 * length(vec2(ratio, 1.0)) + u_softness;
    float p = u_invert ? 1.0 - progress : progress;
    float radius = p * reach;
    float inside = 1.0 - smoothstep(radius - u_softness, radius, d);
    return u_invert ? mix(getToColor(uv), getFromColor(uv), inside)
                    : mix(getFromColor(uv), getToColor(uv), inside);
}
