/* Split from main.c: CRT post-processing effects. Included by main.c. */

static uint32_t app_crt_pack(uint32_t base, uint8_t r, uint8_t g, uint8_t b) {
    return (base & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint8_t app_crt_dim(uint8_t channel) {
    return (uint8_t)(((unsigned)channel * 7u) / 8u);
}

/* Applies the enabled CRT filters to the live framebuffer and returns the
   buffer that should be blitted. The copy/dimming work is skipped entirely
   when scanlines are disabled, so the live framebuffer is returned directly. */
static const uint32_t *app_crt_apply_effects(AppState *app) {
    int width;
    size_t count;
    if (app == NULL) {
        return NULL;
    }
    width = app->machine_kind == APP_MACHINE_NEXT
        ? app->next.framebuffer_width
        : spectrum_framebuffer_width(&app->spec);
    count = (size_t)width * (size_t)ZX_SCREEN_HEIGHT;
    if (app->crt_framebuffer == NULL || !app->crt_scanlines) {
        return app->machine_kind == APP_MACHINE_NEXT
            ? app->next.framebuffer
            : app->spec.framebuffer;
    }
    memcpy(app->crt_framebuffer,
        app->machine_kind == APP_MACHINE_NEXT
            ? app->next.framebuffer
            : app->spec.framebuffer,
        count * sizeof(uint32_t));
    /* Only the even lines are dimmed; the odd ones stay as copied. */
    for (int y = 0; y < ZX_SCREEN_HEIGHT; y += 2) {
        for (int x = 0; x < width; ++x) {
            const size_t index = (size_t)y * (size_t)width + (size_t)x;
            const uint32_t color = app->crt_framebuffer[index];
            const uint8_t r = app_crt_dim((uint8_t)((color >> 16) & 0xFFu));
            const uint8_t g = app_crt_dim((uint8_t)((color >> 8) & 0xFFu));
            const uint8_t b = app_crt_dim((uint8_t)(color & 0xFFu));
            app->crt_framebuffer[index] = app_crt_pack(color, r, g, b);
        }
    }
    return app->crt_framebuffer;
}
