// Invert: the built-in effect of the same name, as a plugin (template for a colour effect).
//
// entry "color": Kinoklip calls apply() for every pixel with the STRAIGHT (not premultiplied)
// colour c in 0..1, in the same encoding as the picture (no conversion to linear light), and
// premultiplies the result again. Transparent pixels stay transparent.
//
// u_amount is declared for you from plugin.json (float, 0..100 percent).
vec3 apply(vec3 c)
{
    float amount = clamp(u_amount / 100.0, 0.0, 1.0);
    return mix(c, 1.0 - c, amount);
}
