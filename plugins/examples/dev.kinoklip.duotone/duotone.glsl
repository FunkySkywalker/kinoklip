// Two-Colour Look: shadows and highlights take two colours.
//
// c is the straight colour of the pixel (0..1); luma() is a helper Kinoklip provides (BT.709).
// u_shadows and u_highlights are vec4 (straight RGBA of the two colour parameters), u_amount
// is the float of the Amount parameter in percent.
vec3 apply(vec3 c)
{
    float y = luma(c);
    vec3 duo = mix(u_shadows.rgb, u_highlights.rgb, y);
    return mix(c, duo, clamp(u_amount / 100.0, 0.0, 1.0));
}
