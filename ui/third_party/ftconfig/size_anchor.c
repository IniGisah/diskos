#include <ft2build.h>
#include FT_FREETYPE_H

/* Retained only in the measurement copy of mq_ui. */
void diskos_ft_size_anchor(void)
{
    FT_Library library;
    if (FT_Init_FreeType(&library) == 0)
        FT_Done_FreeType(library);
}
