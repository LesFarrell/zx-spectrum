/* Split from main.c: assembler core implementation. Included by main.c. */

static char *app_trim_inplace(char *text)
{
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start))
    {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
    {
        end--;
    }
    *end = '\0';
    return start;
}

/* Removes assembler comments introduced by ';' from a source line unless the
   marker appears inside a single- or double-quoted string literal. */
static void app_strip_comment(char *text)
{
    bool in_single = false;
    bool in_double = false;

    for (char *cursor = text; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '\'' && !in_double)
        {
            in_single = !in_single;
        }
        else if (*cursor == '"' && !in_single)
        {
            in_double = !in_double;
        }
        else if (*cursor == ';' && !in_single && !in_double)
        {
            *cursor = '\0';
            return;
        }
    }
}

/* Returns true when two ASCII strings match irrespective of case. */
static bool app_equals_ignore_case(const char *lhs, const char *rhs)
{
    return _stricmp(lhs, rhs) == 0;
}

/* Copies the next token into an upper-case destination buffer and returns the
   number of source characters that were consumed from the input line. */
static size_t app_read_upper_token(const char *text, char *token, size_t token_size)
{
    size_t length = 0;

    while (
        text[length] != '\0' &&
        !isspace((unsigned char)text[length]) &&
        text[length] != ',' &&
        text[length] != '(')
    {
        if (length + 1 < token_size)
        {
            token[length] = (char)toupper((unsigned char)text[length]);
        }
        length++;
    }

    if (token_size > 0)
    {
        size_t copy_length = (length < token_size - 1) ? length : (token_size - 1);
        token[copy_length] = '\0';
    }
    return length;
}

/* Reads a byte from the currently mapped Spectrum address space so the
   debugger can inspect RAM, ROM, and paged banks through one view. */

static int app_read_operand_strict(
    char **text,
    char *operand,
    size_t operand_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    char *cursor;
    char *start;
    bool in_single = false;
    bool in_double = false;

    if (text == NULL || *text == NULL)
    {
        return 0;
    }

    start = *text;
    while (*start != '\0' && isspace((unsigned char)*start))
    {
        start++;
    }
    if (*start == '\0')
    {
        *text = start;
        return 0;
    }
    if (*start == ',')
    {
        snprintf(error_buffer, error_buffer_size, "Missing operand before comma.");
        return -1;
    }

    cursor = start;
    while (*cursor != '\0')
    {
        if (*cursor == '\'' && !in_double)
        {
            in_single = !in_single;
        }
        else if (*cursor == '"' && !in_single)
        {
            in_double = !in_double;
        }
        else if (*cursor == ',' && !in_single && !in_double)
        {
            break;
        }
        cursor++;
    }

    {
        size_t length = (size_t)(cursor - start);
        size_t copy_length = (length < operand_size - 1) ? length : (operand_size - 1);
        memcpy(operand, start, copy_length);
        operand[copy_length] = '\0';
    }
    {
        char *trimmed = app_trim_inplace(operand);
        if (trimmed != operand)
        {
            memmove(operand, trimmed, strlen(trimmed) + 1);
        }
    }
    if (operand[0] == '\0')
    {
        snprintf(error_buffer, error_buffer_size, "Missing operand.");
        return -1;
    }

    if (*cursor == ',')
    {
        char *next = cursor + 1;
        while (*next != '\0' && isspace((unsigned char)*next))
        {
            next++;
        }
        if (*next == '\0')
        {
            snprintf(error_buffer, error_buffer_size, "Missing operand after comma.");
            return -1;
        }
        if (*next == ',')
        {
            snprintf(error_buffer, error_buffer_size, "Missing operand between commas.");
            return -1;
        }
        *text = next;
    }
    else
    {
        *text = cursor;
    }
    return 1;
}

/* Reports a clear syntax error when an instruction receives the wrong number
   of operands for the specific form supported by the mini assembler. */
static bool app_require_operand_count(
    const char *mnemonic,
    int actual_count,
    int min_count,
    int max_count,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (actual_count < min_count || actual_count > max_count)
    {
        if (min_count == max_count)
        {
            snprintf(
                error_buffer,
                error_buffer_size,
                "%s expects %d operand%s.",
                mnemonic,
                min_count,
                min_count == 1 ? "" : "s");
        }
        else
        {
            snprintf(
                error_buffer,
                error_buffer_size,
                "%s expects %d or %d operands.",
                mnemonic,
                min_count,
                max_count);
        }
        return false;
    }
    return true;
}

/* Parses a simple assembler numeric literal in decimal, hex, binary, or as a
   quoted character literal such as `'A'`. */
static bool app_parse_number(const char *text, int *value)
{
    char token[128];
    char *end = NULL;
    long parsed;
    bool negative = false;
    int base = 10;
    size_t length;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token)
        {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }

    length = strlen(token);
    if (length == 0)
    {
        return false;
    }

    if (token[0] == '-' || token[0] == '+')
    {
        negative = token[0] == '-';
        memmove(token, token + 1, strlen(token));
        length = strlen(token);
        if (length == 0)
        {
            return false;
        }
    }

    if ((token[0] == '\'' || token[0] == '"') && length >= 3 && token[length - 1] == token[0])
    {
        if (length != 3)
        {
            return false;
        }
        *value = negative ? -(unsigned char)token[1] : (unsigned char)token[1];
        return true;
    }

    if (token[0] == '$')
    {
        memmove(token, token + 1, strlen(token));
        base = 16;
    }
    else if (length > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
    {
        memmove(token, token + 2, strlen(token) - 1);
        base = 16;
    }
    else if (token[0] == '%')
    {
        memmove(token, token + 1, strlen(token));
        base = 2;
    }
    else if (length > 1 && (token[length - 1] == 'h' || token[length - 1] == 'H'))
    {
        token[length - 1] = '\0';
        base = 16;
    }
    else if (length > 1 && (token[length - 1] == 'b' || token[length - 1] == 'B'))
    {
        token[length - 1] = '\0';
        base = 2;
    }

    parsed = strtol(token, &end, base);
    if (end == NULL || *end != '\0')
    {
        return false;
    }
    if (negative)
    {
        parsed = -parsed;
    }
    *value = (int)parsed;
    return true;
}

/* Returns true when the character is valid at the start of a label name. */
static bool app_is_label_start_char(char ch)
{
    return isalpha((unsigned char)ch) || ch == '_' || ch == '.' || ch == '?';
}

/* Returns true when the character is valid after the first label character. */
static bool app_is_label_char(char ch)
{
    return isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '?' || ch == '$';
}

/* Returns true when a symbol name matches the assembler's identifier rules. */
static bool app_is_symbol_name(const char *text)
{
    if (text == NULL || !app_is_label_start_char(text[0]))
    {
        return false;
    }
    for (size_t i = 1; text[i] != '\0'; ++i)
    {
        if (!app_is_label_char(text[i]))
        {
            return false;
        }
    }
    return true;
}

/* Reports why a symbol name is invalid so assembler diagnostics can point to
   naming rules instead of falling back to unrelated mnemonic errors. */
static void app_format_invalid_symbol_error(
    const char *name,
    char *error_buffer,
    size_t error_buffer_size)
{
    snprintf(
        error_buffer,
        error_buffer_size,
        "Invalid symbol name: %s. Use letters, digits, '_', '.', '?', or '$'; the first character cannot be a digit.",
        name != NULL ? name : "");
}

/* Looks up a previously defined label by name and returns its table index. */
static int app_find_label_index(const AssemblerContext *ctx, const char *name)
{
    for (size_t i = 0; i < ctx->label_count; ++i)
    {
        if (_stricmp(ctx->labels[i].name, name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/* Looks up a previously defined constant by name and returns its table index. */
static int app_find_constant_index(const AssemblerContext *ctx, const char *name)
{
    for (size_t i = 0; i < ctx->constant_count; ++i)
    {
        if (_stricmp(ctx->constants[i].name, name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/* Defines a label at the current assembly address during pass 1 and verifies
   that pass 2 sees the same symbol layout. */
static bool app_define_label(
    AssemblerContext *ctx,
    const char *name,
    uint16_t address,
    char *error_buffer,
    size_t error_buffer_size)
{
    int existing_index = app_find_label_index(ctx, name);

    if (ctx->pass == 1)
    {
        if (existing_index >= 0)
        {
            snprintf(error_buffer, error_buffer_size, "Duplicate label: %s", name);
            return false;
        }
        if (app_find_constant_index(ctx, name) >= 0)
        {
            snprintf(error_buffer, error_buffer_size, "Symbol already defined as a constant: %s", name);
            return false;
        }
        if (ctx->label_count >= sizeof(ctx->labels) / sizeof(ctx->labels[0]))
        {
            snprintf(error_buffer, error_buffer_size, "Too many labels in assembler source.");
            return false;
        }
        snprintf(ctx->labels[ctx->label_count].name, sizeof(ctx->labels[ctx->label_count].name), "%s", name);
        ctx->labels[ctx->label_count].address = address;
        ctx->label_count++;
        return true;
    }

    if (existing_index < 0)
    {
        snprintf(error_buffer, error_buffer_size, "Unknown label during second pass: %s", name);
        return false;
    }
    if (ctx->labels[existing_index].address != address)
    {
        snprintf(error_buffer, error_buffer_size, "Label address changed between passes: %s", name);
        return false;
    }
    return true;
}

/* Defines a named constant during pass 1 and verifies pass 2 sees the same
   value so `EQU` symbols stay stable across both assembly passes. */
static bool app_define_constant(
    AssemblerContext *ctx,
    const char *name,
    int value,
    char *error_buffer,
    size_t error_buffer_size)
{
    int existing_index = app_find_constant_index(ctx, name);

    if (ctx->pass == 1)
    {
        if (existing_index >= 0)
        {
            snprintf(error_buffer, error_buffer_size, "Duplicate constant: %s", name);
            return false;
        }
        if (app_find_label_index(ctx, name) >= 0)
        {
            snprintf(error_buffer, error_buffer_size, "Symbol already defined as a label: %s", name);
            return false;
        }
        if (ctx->constant_count >= sizeof(ctx->constants) / sizeof(ctx->constants[0]))
        {
            snprintf(error_buffer, error_buffer_size, "Too many constants in assembler source.");
            return false;
        }
        snprintf(ctx->constants[ctx->constant_count].name, sizeof(ctx->constants[ctx->constant_count].name), "%s", name);
        ctx->constants[ctx->constant_count].value = value;
        ctx->constant_count++;
        return true;
    }

    if (existing_index < 0)
    {
        snprintf(error_buffer, error_buffer_size, "Unknown constant during second pass: %s", name);
        return false;
    }
    if (ctx->constants[existing_index].value != value)
    {
        snprintf(error_buffer, error_buffer_size, "Constant value changed between passes: %s", name);
        return false;
    }
    return true;
}

/* Parses a traditional `NAME EQU value` constant definition and adds it to
   the active symbol table when present on the current line. */
static bool app_parse_equ_definition(
    AssemblerContext *ctx,
    char *text,
    bool *handled,
    char *error_buffer,
    size_t error_buffer_size)
{
    char name[64];
    char directive[32];
    char operand[256];
    AssemblerValue value;
    char *cursor;
    char *operand_cursor;
    size_t name_chars;
    size_t directive_chars;
    int operand_result;

    if (handled != NULL)
    {
        *handled = false;
    }
    if (ctx == NULL || text == NULL)
    {
        return false;
    }

    name_chars = app_read_upper_token(text, name, sizeof(name));
    if (name_chars == 0)
    {
        return true;
    }

    cursor = text + name_chars;
    if (*cursor == '\0' || !isspace((unsigned char)*cursor))
    {
        return true;
    }
    while (*cursor != '\0' && isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    directive_chars = app_read_upper_token(cursor, directive, sizeof(directive));
    if (directive_chars == 0 || !app_equals_ignore_case(directive, "EQU"))
    {
        return true;
    }
    if (handled != NULL)
    {
        *handled = true;
    }
    if (!app_is_symbol_name(name))
    {
        app_format_invalid_symbol_error(name, error_buffer, error_buffer_size);
        return false;
    }

    operand_cursor = cursor + directive_chars;
    operand_result = app_read_operand_strict(&operand_cursor, operand, sizeof(operand), error_buffer, error_buffer_size);
    if (operand_result <= 0)
    {
        if (operand_result == 0)
        {
            snprintf(error_buffer, error_buffer_size, "EQU expects 1 operand.");
        }
        return false;
    }
    if (app_read_operand_strict(&operand_cursor, directive, sizeof(directive), error_buffer, error_buffer_size) > 0)
    {
        snprintf(error_buffer, error_buffer_size, "EQU expects 1 operand.");
        return false;
    }
    if (!app_parse_value(ctx, operand, &value))
    {
        snprintf(error_buffer, error_buffer_size, "Invalid EQU value: %s", operand);
        return false;
    }
    if (!value.resolved)
    {
        snprintf(error_buffer, error_buffer_size, "EQU requires an already-defined value: %s", operand);
        return false;
    }
    return app_define_constant(ctx, name, value.value, error_buffer, error_buffer_size);
}

/* Consumes one leading `label:` definition from the current line if present. */
static bool app_parse_leading_label(char **text, char *label, size_t label_size)
{
    char *cursor;
    char *name_end;
    size_t length;

    if (text == NULL || *text == NULL)
    {
        return false;
    }

    cursor = *text;
    if (!app_is_label_start_char(*cursor))
    {
        return false;
    }

    name_end = cursor + 1;
    while (app_is_label_char(*name_end))
    {
        name_end++;
    }
    length = (size_t)(name_end - cursor);

    while (*name_end != '\0' && isspace((unsigned char)*name_end))
    {
        name_end++;
    }
    if (*name_end != ':')
    {
        return false;
    }

    if (label_size > 0)
    {
        size_t copy_length = (length < label_size - 1) ? length : (label_size - 1);
        memcpy(label, cursor, copy_length);
        label[copy_length] = '\0';
    }

    *text = name_end + 1;
    return true;
}

/* Scans assembler source for the first explicit ORG directive so assembly can
   start from source-defined addresses without separate UI state. */
static bool app_assembler_find_source_org(const char *source, uint16_t *out_address)
{
    AssemblerContext ctx;
    char *mutable_source;
    char *cursor;
    char *line;

    memset(&ctx, 0, sizeof(ctx));
    ctx.pass = 1;

    mutable_source = _strdup(source);
    if (mutable_source == NULL)
    {
        return false;
    }

    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0')
    {
        char *line_end = strchr(cursor, '\n');
        char mnemonic[32];
        char operand[128];
        char label[64];
        char *text;
        size_t mnemonic_chars;
        int org_value;

        line = cursor;
        if (line_end != NULL)
        {
            *line_end = '\0';
            cursor = line_end + 1;
        }
        else
        {
            cursor = NULL;
        }

        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r')
            {
                line[line_length - 1] = '\0';
            }
        }

        app_strip_comment(line);
        text = app_trim_inplace(line);
        if (*text == '\0')
        {
            continue;
        }
        while (app_parse_leading_label(&text, label, sizeof(label)))
        {
            text = app_trim_inplace(text);
            if (*text == '\0')
            {
                break;
            }
        }
        if (*text == '\0')
        {
            continue;
        }
        {
            bool handled_equ = false;
            char scan_error[256];

            if (!app_parse_equ_definition(&ctx, text, &handled_equ, scan_error, sizeof(scan_error)))
            {
                free(mutable_source);
                return false;
            }
            if (handled_equ)
            {
                continue;
            }
        }

        mnemonic_chars = app_read_upper_token(text, mnemonic, sizeof(mnemonic));
        text += mnemonic_chars;
        text = app_trim_inplace(text);
        if (!app_equals_ignore_case(mnemonic, "ORG"))
        {
            continue;
        }
        operand[0] = '\0';
        if (*text != '\0')
        {
            char *operand_cursor = text;
            if (app_read_operand_strict(&operand_cursor, operand, sizeof(operand), mnemonic, sizeof(mnemonic)) <= 0)
            {
                continue;
            }
        }
        {
            AssemblerValue value;

            if (!app_parse_value(&ctx, operand, &value) || !value.resolved)
            {
                continue;
            }
            org_value = value.value;
        }
        if (org_value >= 0 && org_value <= 0xFFFF)
        {
            *out_address = (uint16_t)org_value;
            free(mutable_source);
            return true;
        }
    }

    free(mutable_source);
    return false;
}

/* Resolves an assembler value token as either a numeric literal or a label.
   During pass 1 forward label references are accepted as unresolved zeros. */
static bool app_parse_value(
    const AssemblerContext *ctx,
    const char *text,
    AssemblerValue *out_value)
{
    char token[128];
    int numeric_value;
    int constant_index;
    int label_index;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token)
        {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }
    if (token[0] == '\0')
    {
        return false;
    }

    if (app_parse_number(token, &numeric_value))
    {
        out_value->value = numeric_value;
        out_value->resolved = true;
        out_value->numeric_literal = true;
        return true;
    }

    if (!app_is_symbol_name(token))
    {
        return false;
    }

    constant_index = app_find_constant_index(ctx, token);
    if (constant_index >= 0)
    {
        out_value->value = ctx->constants[constant_index].value;
        out_value->resolved = true;
        out_value->numeric_literal = false;
        return true;
    }

    label_index = app_find_label_index(ctx, token);
    if (label_index >= 0)
    {
        out_value->value = ctx->labels[label_index].address;
        out_value->resolved = true;
        out_value->numeric_literal = false;
        return true;
    }

    if (ctx->pass == 1)
    {
        out_value->value = 0;
        out_value->resolved = false;
        out_value->numeric_literal = false;
        return true;
    }
    return false;
}

/* Returns the 8-bit register code used by most primary Z80 opcodes, including
   `(HL)` for memory-indirect forms. */
static int app_parse_reg8(const char *text)
{
    static const char *names[] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    if (app_equals_ignore_case(text, "(HL)"))
    {
        return 6;
    }
    for (int i = 0; i < 8; ++i)
    {
        if (app_equals_ignore_case(text, names[i]))
        {
            return i;
        }
    }
    return -1;
}

/* Returns the standard BC/DE/HL/SP register-pair code used by many 16-bit
   Z80 instructions. */
static int app_parse_reg16(const char *text)
{
    static const char *names[] = {"BC", "DE", "HL", "SP"};
    for (int i = 0; i < 4; ++i)
    {
        if (app_equals_ignore_case(text, names[i]))
        {
            return i;
        }
    }
    return -1;
}

/* Returns the BC/DE/HL/AF register-pair code used by PUSH and POP. */
static int app_parse_reg16_push(const char *text)
{
    static const char *names[] = {"BC", "DE", "HL", "AF"};
    for (int i = 0; i < 4; ++i)
    {
        if (app_equals_ignore_case(text, names[i]))
        {
            return i;
        }
    }
    return -1;
}

/* Maps a condition mnemonic such as `NZ` or `C` to its Z80 condition code. */
static int app_parse_condition(const char *text)
{
    static const char *names[] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
    for (int i = 0; i < 8; ++i)
    {
        if (app_equals_ignore_case(text, names[i]))
        {
            return i;
        }
    }
    return -1;
}

/* Parses a `(nn)` absolute memory operand and returns the enclosed address. */
static bool app_parse_indirect_address(
    const AssemblerContext *ctx,
    const char *text,
    AssemblerValue *value)
{
    char token[128];
    char *inner;
    size_t length;

    snprintf(token, sizeof(token), "%s", text);
    {
        char *trimmed = app_trim_inplace(token);
        if (trimmed != token)
        {
            memmove(token, trimmed, strlen(trimmed) + 1);
        }
    }

    length = strlen(token);
    if (length < 3 || token[0] != '(' || token[length - 1] != ')')
    {
        return false;
    }
    token[length - 1] = '\0';
    inner = app_trim_inplace(token + 1);

    /* `(nn)` is the absolute-memory form. Register-indirect operands such as
       `(HL)` and `(BC)` must fall through to their dedicated instruction
       encodings instead of being treated as forward label references. */
    if (
        app_equals_ignore_case(inner, "HL") ||
        app_equals_ignore_case(inner, "BC") ||
        app_equals_ignore_case(inner, "DE") ||
        app_equals_ignore_case(inner, "SP") ||
        app_equals_ignore_case(inner, "IX") ||
        app_equals_ignore_case(inner, "IY"))
    {
        return false;
    }

    return app_parse_value(ctx, inner, value);
}

/* Emits bytes into the active Spectrum address space while rejecting writes to
   ROM or beyond the 16-bit address range. */
static bool app_assemble_write_bytes(
    AssemblerContext *ctx,
    const uint8_t *bytes,
    size_t length,
    char *error_buffer,
    size_t error_buffer_size)
{
    if (length == 0)
    {
        return true;
    }
    if (ctx->address < 0x4000)
    {
        snprintf(error_buffer, error_buffer_size, "Assembler writes are limited to RAM at 4000h-FFFFh.");
        return false;
    }
    if ((size_t)ctx->address + length > 0x10000u)
    {
        snprintf(error_buffer, error_buffer_size, "Assembled bytes run past FFFFh.");
        return false;
    }
    if (ctx->pass == 2 && ctx->output != NULL)
    {
        size_t required;
        size_t expected_address;
        uint8_t *new_bytes;

        if (!ctx->output->has_start_address)
        {
            ctx->output->start_address = ctx->address;
            ctx->output->has_start_address = true;
        }
        else
        {
            expected_address = (size_t)ctx->output->start_address + ctx->output->length;
            if (expected_address != (size_t)ctx->address)
            {
                snprintf(
                    error_buffer,
                    error_buffer_size,
                    "TAP export requires one contiguous output range without ORG jumps.");
                return false;
            }
        }

        required = ctx->output->length + length;
        if (required > ctx->output->capacity)
        {
            size_t new_capacity = ctx->output->capacity > 0 ? ctx->output->capacity : 256;
            while (new_capacity < required)
            {
                new_capacity *= 2;
            }
            new_bytes = (uint8_t *)realloc(ctx->output->bytes, new_capacity);
            if (new_bytes == NULL)
            {
                snprintf(error_buffer, error_buffer_size, "Out of memory.");
                return false;
            }
            ctx->output->bytes = new_bytes;
            ctx->output->capacity = new_capacity;
        }
        memcpy(ctx->output->bytes + ctx->output->length, bytes, length);
        ctx->output->length += length;
    }
    if (ctx->pass == 2 && ctx->write_to_machine)
    {
        mem_write_range(&ctx->app->spec.machine.mem, ctx->address, bytes, (uint32_t)length);
    }
    ctx->address = (uint16_t)(ctx->address + (uint16_t)length);
    return true;
}

/* Emits one repeated byte value for large DEFS-style reservations without
   relying on the per-line scratch buffer size. */
static bool app_assemble_fill_bytes(
    AssemblerContext *ctx,
    size_t count,
    uint8_t fill_value,
    char *error_buffer,
    size_t error_buffer_size)
{
    uint8_t chunk[256];
    size_t remaining = count;

    memset(chunk, fill_value, sizeof(chunk));
    while (remaining > 0)
    {
        size_t chunk_size = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (!app_assemble_write_bytes(ctx, chunk, chunk_size, error_buffer, error_buffer_size))
        {
            return false;
        }
        remaining -= chunk_size;
    }
    return true;
}

/* Parses a `DB` item, allowing both numeric literals and quoted ASCII
   strings, and appends the resulting bytes to the output buffer. */
static bool app_assemble_db_item(
    const AssemblerContext *ctx,
    const char *operand,
    uint8_t *bytes,
    size_t *length,
    size_t max_length,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t operand_length = strlen(operand);
    AssemblerValue value;

    if (operand_length >= 2 && (operand[0] == '\'' || operand[0] == '"') && operand[operand_length - 1] == operand[0])
    {
        for (size_t i = 1; i + 1 < operand_length; ++i)
        {
            if (*length >= max_length)
            {
                snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
                return false;
            }
            bytes[(*length)++] = (uint8_t)operand[i];
        }
        return true;
    }

    if (!app_parse_value(ctx, operand, &value))
    {
        snprintf(error_buffer, error_buffer_size, "Invalid DB value: %s", operand);
        return false;
    }
    if (ctx->pass == 2 && (value.value < -128 || value.value > 255))
    {
        snprintf(error_buffer, error_buffer_size, "Invalid DB value: %s", operand);
        return false;
    }
    if (*length >= max_length)
    {
        snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
        return false;
    }
    bytes[(*length)++] = (uint8_t)value.value;
    return true;
}

/* Encodes one assembler line from the mini-assembler supported by the UI. */
static bool app_assemble_line(
    AssemblerContext *ctx,
    char *line,
    const AssemblerSourceLocation *location,
    char *error_buffer,
    size_t error_buffer_size)
{
    AssemblerValue value;
    AssemblerValue fill;
    char mnemonic[32];
    char label[64];
    char lhs[128];
    char rhs[128];
    char extra[128];
    char *operands;
    uint8_t *file_data = NULL;
    uint8_t bytes[256];
    size_t file_size = 0;
    size_t length = 0;
    size_t mnemonic_chars;
    int operand_count = 0;
    int operand_result;

    app_strip_comment(line);
    operands = app_trim_inplace(line);
    if (*operands == '\0')
    {
        return true;
    }

    while (app_parse_leading_label(&operands, label, sizeof(label)))
    {
        if (!app_define_label(ctx, label, ctx->address, error_buffer, error_buffer_size))
        {
            return false;
        }
        operands = app_trim_inplace(operands);
        if (*operands == '\0')
        {
            return true;
        }
    }

    {
        bool handled_equ = false;

        if (!app_parse_equ_definition(ctx, operands, &handled_equ, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (handled_equ)
        {
            return true;
        }
    }

    mnemonic_chars = app_read_upper_token(operands, mnemonic, sizeof(mnemonic));
    operands += mnemonic_chars;
    operands = app_trim_inplace(operands);
    lhs[0] = '\0';
    rhs[0] = '\0';
    extra[0] = '\0';
    if (*operands != '\0' && !app_equals_ignore_case(mnemonic, "DB") && !app_equals_ignore_case(mnemonic, "DW"))
    {
        char *operand_cursor = operands;
        operand_result = app_read_operand_strict(&operand_cursor, lhs, sizeof(lhs), error_buffer, error_buffer_size);
        if (operand_result < 0)
        {
            return false;
        }
        if (operand_result > 0)
        {
            operand_count = 1;
            operand_result = app_read_operand_strict(&operand_cursor, rhs, sizeof(rhs), error_buffer, error_buffer_size);
            if (operand_result < 0)
            {
                return false;
            }
            if (operand_result > 0)
            {
                operand_count = 2;
                operand_result = app_read_operand_strict(&operand_cursor, extra, sizeof(extra), error_buffer, error_buffer_size);
                if (operand_result < 0)
                {
                    return false;
                }
                if (operand_result > 0)
                {
                    snprintf(error_buffer, error_buffer_size, "Too many operands for %s.", mnemonic);
                    return false;
                }
            }
        }
    }

    if (app_equals_ignore_case(mnemonic, "ORG"))
    {
        if (!app_require_operand_count("ORG", operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_parse_value(ctx, lhs, &value) || !value.resolved || value.value < 0 || value.value > 0xFFFF)
        {
            snprintf(error_buffer, error_buffer_size, "Invalid ORG address: %s", lhs);
            return false;
        }
        ctx->address = (uint16_t)value.value;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "DB"))
    {
        char *cursor = operands;
        char operand[256];
        operand_count = 0;
        while ((operand_result = app_read_operand_strict(&cursor, operand, sizeof(operand), error_buffer, error_buffer_size)) > 0)
        {
            operand_count++;
            if (!app_assemble_db_item(ctx, operand, bytes, &length, sizeof(bytes), error_buffer, error_buffer_size))
            {
                return false;
            }
        }
        if (operand_result < 0)
        {
            return false;
        }
        if (operand_count == 0)
        {
            snprintf(error_buffer, error_buffer_size, "DB expects at least 1 operand.");
            return false;
        }
        if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size))
        {
            return false;
        }
        ctx->total_written += length;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "DW"))
    {
        char *cursor = operands;
        char operand[256];
        operand_count = 0;
        while ((operand_result = app_read_operand_strict(&cursor, operand, sizeof(operand), error_buffer, error_buffer_size)) > 0)
        {
            operand_count++;
            if (!app_parse_value(ctx, operand, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid DW value: %s", operand);
                return false;
            }
            if (ctx->pass == 2 && (value.value < -32768 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid DW value: %s", operand);
                return false;
            }
            if (length + 2 > sizeof(bytes))
            {
                snprintf(error_buffer, error_buffer_size, "Line expands beyond the assembler output buffer.");
                return false;
            }
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        if (operand_result < 0)
        {
            return false;
        }
        if (operand_count == 0)
        {
            snprintf(error_buffer, error_buffer_size, "DW expects at least 1 operand.");
            return false;
        }
        if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size))
        {
            return false;
        }
        ctx->total_written += length;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "DEFS") || app_equals_ignore_case(mnemonic, "DS"))
    {
        size_t reserve_count;
        uint8_t fill_value = 0;

        if (!app_require_operand_count(mnemonic, operand_count, 1, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_parse_value(ctx, lhs, &value) || !value.resolved || value.value < 0)
        {
            snprintf(error_buffer, error_buffer_size, "%s requires an already-defined non-negative count: %s", mnemonic, lhs);
            return false;
        }
        reserve_count = (size_t)value.value;
        if (rhs[0] != '\0')
        {
            if (!app_parse_value(ctx, rhs, &fill))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid %s fill value: %s", mnemonic, rhs);
                return false;
            }
            if (ctx->pass == 2 && (!fill.resolved || fill.value < -128 || fill.value > 255))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid %s fill value: %s", mnemonic, rhs);
                return false;
            }
            fill_value = (uint8_t)fill.value;
        }
        if (!app_assemble_fill_bytes(ctx, reserve_count, fill_value, error_buffer, error_buffer_size))
        {
            return false;
        }
        ctx->total_written += reserve_count;
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "INCBIN"))
    {
        char path[MAX_PATH];

        if (!app_require_operand_count("INCBIN", operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_assembler_resolve_source_path(
                location != NULL ? location->path : NULL,
                lhs,
                "INCBIN",
                path,
                sizeof(path),
                error_buffer,
                error_buffer_size))
        {
            return false;
        }
        if (!app_read_file_all(path, &file_data, &file_size, error_buffer, error_buffer_size, "binary"))
        {
            return false;
        }
        if (!app_assemble_write_bytes(ctx, file_data, file_size, error_buffer, error_buffer_size))
        {
            free(file_data);
            return false;
        }
        ctx->total_written += file_size;
        free(file_data);
        return true;
    }

    if (app_equals_ignore_case(mnemonic, "NOP"))
    {
        if (!app_require_operand_count("NOP", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x00;
    }
    else if (app_equals_ignore_case(mnemonic, "HALT"))
    {
        if (!app_require_operand_count("HALT", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x76;
    }
    else if (app_equals_ignore_case(mnemonic, "DI"))
    {
        if (!app_require_operand_count("DI", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0xF3;
    }
    else if (app_equals_ignore_case(mnemonic, "EI"))
    {
        if (!app_require_operand_count("EI", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0xFB;
    }
    else if (app_equals_ignore_case(mnemonic, "SCF"))
    {
        if (!app_require_operand_count("SCF", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x37;
    }
    else if (app_equals_ignore_case(mnemonic, "CCF"))
    {
        if (!app_require_operand_count("CCF", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x3F;
    }
    else if (app_equals_ignore_case(mnemonic, "CPL"))
    {
        if (!app_require_operand_count("CPL", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x2F;
    }
    else if (app_equals_ignore_case(mnemonic, "DAA"))
    {
        if (!app_require_operand_count("DAA", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x27;
    }
    else if (app_equals_ignore_case(mnemonic, "RLCA"))
    {
        if (!app_require_operand_count("RLCA", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x07;
    }
    else if (app_equals_ignore_case(mnemonic, "RRCA"))
    {
        if (!app_require_operand_count("RRCA", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x0F;
    }
    else if (app_equals_ignore_case(mnemonic, "RLA"))
    {
        if (!app_require_operand_count("RLA", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x17;
    }
    else if (app_equals_ignore_case(mnemonic, "RRA"))
    {
        if (!app_require_operand_count("RRA", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0x1F;
    }
    else if (app_equals_ignore_case(mnemonic, "EXX"))
    {
        if (!app_require_operand_count("EXX", operand_count, 0, 0, error_buffer, error_buffer_size))
        {
            return false;
        }
        bytes[length++] = 0xD9;
    }
    else if (app_equals_ignore_case(mnemonic, "RET"))
    {
        if (!app_require_operand_count("RET", operand_count, 0, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (lhs[0] == '\0')
        {
            bytes[length++] = 0xC9;
        }
        else
        {
            int cc = app_parse_condition(lhs);
            if (cc < 0)
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported RET condition: %s", lhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC0 + (cc << 3));
        }
    }
    else if (app_equals_ignore_case(mnemonic, "RST"))
    {
        int rst_value;
        if (!app_require_operand_count("RST", operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_parse_number(lhs, &rst_value) || rst_value < 0 || rst_value > 0x38 || (rst_value & 0x07) != 0)
        {
            snprintf(error_buffer, error_buffer_size, "RST expects 00h,08h,...,38h.");
            return false;
        }
        bytes[length++] = (uint8_t)(0xC7 + rst_value);
    }
    else if (app_equals_ignore_case(mnemonic, "DJNZ"))
    {
        int disp;
        if (!app_require_operand_count("DJNZ", operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_parse_value(ctx, lhs, &value))
        {
            snprintf(error_buffer, error_buffer_size, "Invalid DJNZ target: %s", lhs);
            return false;
        }
        disp = (value.numeric_literal && value.value >= -128 && value.value <= 127)
                   ? value.value
                   : (value.value - ((int)ctx->address + 2));
        if (ctx->pass == 2 && (disp < -128 || disp > 127))
        {
            snprintf(error_buffer, error_buffer_size, "DJNZ target is out of range.");
            return false;
        }
        bytes[length++] = 0x10;
        bytes[length++] = (uint8_t)(int8_t)disp;
    }
    else if (app_equals_ignore_case(mnemonic, "JR"))
    {
        int cc = -1;
        int disp;
        if (!app_require_operand_count("JR", operand_count, 1, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (rhs[0] != '\0')
        {
            cc = app_parse_condition(lhs);
            if (cc < 0 || cc > 3)
            {
                snprintf(error_buffer, error_buffer_size, "JR only supports NZ, Z, NC, or C.");
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid JR target: %s", rhs);
                return false;
            }
        }
        else
        {
            if (!app_parse_value(ctx, lhs, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid JR target: %s", lhs);
                return false;
            }
        }
        disp = (value.numeric_literal && value.value >= -128 && value.value <= 127)
                   ? value.value
                   : (value.value - ((int)ctx->address + 2));
        if (ctx->pass == 2 && (disp < -128 || disp > 127))
        {
            snprintf(error_buffer, error_buffer_size, "JR target is out of range.");
            return false;
        }
        bytes[length++] = (uint8_t)((cc >= 0) ? (0x20 + (cc << 3)) : 0x18);
        bytes[length++] = (uint8_t)(int8_t)disp;
    }
    else if (app_equals_ignore_case(mnemonic, "JP"))
    {
        if (!app_require_operand_count("JP", operand_count, 1, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (rhs[0] == '\0')
        {
            if (app_equals_ignore_case(lhs, "(HL)"))
            {
                bytes[length++] = 0xE9;
            }
            else if (app_equals_ignore_case(lhs, "(IX)"))
            {
                bytes[length++] = 0xDD;
                bytes[length++] = 0xE9;
            }
            else if (app_equals_ignore_case(lhs, "(IY)"))
            {
                bytes[length++] = 0xFD;
                bytes[length++] = 0xE9;
            }
            else
            {
                if (!app_parse_value(ctx, lhs, &value))
                {
                    snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", lhs);
                    return false;
                }
                if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
                {
                    snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", lhs);
                    return false;
                }
                bytes[length++] = 0xC3;
                bytes[length++] = (uint8_t)value.value;
                bytes[length++] = (uint8_t)(value.value >> 8);
            }
        }
        else
        {
            int cc = app_parse_condition(lhs);
            if (cc < 0)
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported JP condition: %s", lhs);
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", rhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid JP target: %s", rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC2 + (cc << 3));
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
    }
    else if (app_equals_ignore_case(mnemonic, "CALL"))
    {
        if (!app_require_operand_count("CALL", operand_count, 1, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (rhs[0] == '\0')
        {
            if (!app_parse_value(ctx, lhs, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", lhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", lhs);
                return false;
            }
            bytes[length++] = 0xCD;
        }
        else
        {
            int cc = app_parse_condition(lhs);
            if (cc < 0)
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported CALL condition: %s", lhs);
                return false;
            }
            if (!app_parse_value(ctx, rhs, &value))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", rhs);
                return false;
            }
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Invalid CALL target: %s", rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0xC4 + (cc << 3));
        }
        bytes[length++] = (uint8_t)value.value;
        bytes[length++] = (uint8_t)(value.value >> 8);
    }
    else if (app_equals_ignore_case(mnemonic, "OUT"))
    {
        if (!app_require_operand_count("OUT", operand_count, 2, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_equals_ignore_case(rhs, "A") || !app_parse_indirect_address(ctx, lhs, &value))
        {
            snprintf(error_buffer, error_buffer_size, "Only OUT (n),A is supported.");
            return false;
        }
        if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFF))
        {
            snprintf(error_buffer, error_buffer_size, "Only OUT (n),A is supported.");
            return false;
        }
        bytes[length++] = 0xD3;
        bytes[length++] = (uint8_t)value.value;
    }
    else if (app_equals_ignore_case(mnemonic, "IN"))
    {
        if (!app_require_operand_count("IN", operand_count, 2, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (!app_equals_ignore_case(lhs, "A") || !app_parse_indirect_address(ctx, rhs, &value))
        {
            snprintf(error_buffer, error_buffer_size, "Only IN A,(n) is supported.");
            return false;
        }
        if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFF))
        {
            snprintf(error_buffer, error_buffer_size, "Only IN A,(n) is supported.");
            return false;
        }
        bytes[length++] = 0xDB;
        bytes[length++] = (uint8_t)value.value;
    }
    else if (app_equals_ignore_case(mnemonic, "PUSH") || app_equals_ignore_case(mnemonic, "POP"))
    {
        int rr = app_parse_reg16_push(lhs);
        uint8_t base = app_equals_ignore_case(mnemonic, "PUSH") ? 0xC5 : 0xC1;
        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (rr >= 0)
        {
            bytes[length++] = (uint8_t)(base + (rr << 4));
        }
        else if (app_equals_ignore_case(lhs, "IX"))
        {
            bytes[length++] = 0xDD;
            bytes[length++] = (uint8_t)(app_equals_ignore_case(mnemonic, "PUSH") ? 0xE5 : 0xE1);
        }
        else if (app_equals_ignore_case(lhs, "IY"))
        {
            bytes[length++] = 0xFD;
            bytes[length++] = (uint8_t)(app_equals_ignore_case(mnemonic, "PUSH") ? 0xE5 : 0xE1);
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported register for %s: %s", mnemonic, lhs);
            return false;
        }
    }
    else if (app_equals_ignore_case(mnemonic, "INC") || app_equals_ignore_case(mnemonic, "DEC"))
    {
        uint8_t op8 = app_equals_ignore_case(mnemonic, "INC") ? 0x04 : 0x05;
        uint8_t op16 = app_equals_ignore_case(mnemonic, "INC") ? 0x03 : 0x0B;
        int r = app_parse_reg8(lhs);
        int rr = app_parse_reg16(lhs);
        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (r >= 0)
        {
            bytes[length++] = (uint8_t)(op8 + (r << 3));
        }
        else if (rr >= 0)
        {
            bytes[length++] = (uint8_t)(op16 + (rr << 4));
        }
        else if (app_equals_ignore_case(lhs, "IX"))
        {
            bytes[length++] = 0xDD;
            bytes[length++] = app_equals_ignore_case(mnemonic, "INC") ? 0x23 : 0x2B;
        }
        else if (app_equals_ignore_case(lhs, "IY"))
        {
            bytes[length++] = 0xFD;
            bytes[length++] = app_equals_ignore_case(mnemonic, "INC") ? 0x23 : 0x2B;
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
            return false;
        }
    }
    else if (app_equals_ignore_case(mnemonic, "EX"))
    {
        if (!app_require_operand_count("EX", operand_count, 2, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (app_equals_ignore_case(lhs, "DE") && app_equals_ignore_case(rhs, "HL"))
        {
            bytes[length++] = 0xEB;
        }
        else if (app_equals_ignore_case(lhs, "AF") && app_equals_ignore_case(rhs, "AF'"))
        {
            bytes[length++] = 0x08;
        }
        else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "HL"))
        {
            bytes[length++] = 0xE3;
        }
        else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "IX"))
        {
            bytes[length++] = 0xDD;
            bytes[length++] = 0xE3;
        }
        else if (app_equals_ignore_case(lhs, "(SP)") && app_equals_ignore_case(rhs, "IY"))
        {
            bytes[length++] = 0xFD;
            bytes[length++] = 0xE3;
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported EX form.");
            return false;
        }
    }
    else if (app_equals_ignore_case(mnemonic, "ADD"))
    {
        int rr = app_parse_reg16(rhs);
        int r = app_parse_reg8(rhs);
        if (!app_require_operand_count("ADD", operand_count, 2, 2, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (app_equals_ignore_case(lhs, "HL") && rr >= 0)
        {
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        }
        else if (app_equals_ignore_case(lhs, "IX") && rr >= 0)
        {
            bytes[length++] = 0xDD;
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        }
        else if (app_equals_ignore_case(lhs, "IY") && rr >= 0)
        {
            bytes[length++] = 0xFD;
            bytes[length++] = (uint8_t)(0x09 + (rr << 4));
        }
        else if (app_equals_ignore_case(lhs, "A") && r >= 0)
        {
            bytes[length++] = (uint8_t)(0x80 + r);
        }
        else if (app_equals_ignore_case(lhs, "A") && app_parse_value(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported ADD form.");
                return false;
            }
            bytes[length++] = 0xC6;
            bytes[length++] = (uint8_t)value.value;
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported ADD form.");
            return false;
        }
    }
    else if (
        app_equals_ignore_case(mnemonic, "SUB") ||
        app_equals_ignore_case(mnemonic, "AND") ||
        app_equals_ignore_case(mnemonic, "OR") ||
        app_equals_ignore_case(mnemonic, "XOR") ||
        app_equals_ignore_case(mnemonic, "CP"))
    {
        uint8_t reg_base;
        uint8_t imm_base;
        int r = app_parse_reg8(lhs);

        if (app_equals_ignore_case(mnemonic, "SUB"))
        {
            reg_base = 0x90;
            imm_base = 0xD6;
        }
        else if (app_equals_ignore_case(mnemonic, "AND"))
        {
            reg_base = 0xA0;
            imm_base = 0xE6;
        }
        else if (app_equals_ignore_case(mnemonic, "XOR"))
        {
            reg_base = 0xA8;
            imm_base = 0xEE;
        }
        else if (app_equals_ignore_case(mnemonic, "OR"))
        {
            reg_base = 0xB0;
            imm_base = 0xF6;
        }
        else
        {
            reg_base = 0xB8;
            imm_base = 0xFE;
        }

        if (!app_require_operand_count(mnemonic, operand_count, 1, 1, error_buffer, error_buffer_size))
        {
            return false;
        }
        if (r >= 0)
        {
            bytes[length++] = (uint8_t)(reg_base + r);
        }
        else if (app_parse_value(ctx, lhs, &value))
        {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
                return false;
            }
            bytes[length++] = imm_base;
            bytes[length++] = (uint8_t)value.value;
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported %s operand: %s", mnemonic, lhs);
            return false;
        }
    }
    else if (app_equals_ignore_case(mnemonic, "LD"))
    {
        int rr = app_parse_reg16(lhs);
        int dst8 = app_parse_reg8(lhs);
        int src8 = app_parse_reg8(rhs);
        if (!app_require_operand_count("LD", operand_count, 2, 2, error_buffer, error_buffer_size))
        {
            return false;
        }

        if (rr >= 0 && app_parse_value(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0x01 + (rr << 4));
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "IX") && app_parse_value(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x21;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "IY") && app_parse_value(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x21;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "HL"))
        {
            bytes[length++] = 0xF9;
        }
        else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "IX"))
        {
            bytes[length++] = 0xDD;
            bytes[length++] = 0xF9;
        }
        else if (app_equals_ignore_case(lhs, "SP") && app_equals_ignore_case(rhs, "IY"))
        {
            bytes[length++] = 0xFD;
            bytes[length++] = 0xF9;
        }
        else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "A"))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x32;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "A") && app_parse_indirect_address(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x3A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "HL"))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "HL") && app_parse_indirect_address(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "IX"))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "IX") && app_parse_indirect_address(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xDD;
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_parse_indirect_address(ctx, lhs, &value) && app_equals_ignore_case(rhs, "IY"))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x22;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "IY") && app_parse_indirect_address(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < 0 || value.value > 0xFFFF))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = 0xFD;
            bytes[length++] = 0x2A;
            bytes[length++] = (uint8_t)value.value;
            bytes[length++] = (uint8_t)(value.value >> 8);
        }
        else if (app_equals_ignore_case(lhs, "(BC)") && app_equals_ignore_case(rhs, "A"))
        {
            bytes[length++] = 0x02;
        }
        else if (app_equals_ignore_case(lhs, "(DE)") && app_equals_ignore_case(rhs, "A"))
        {
            bytes[length++] = 0x12;
        }
        else if (app_equals_ignore_case(lhs, "A") && app_equals_ignore_case(rhs, "(BC)"))
        {
            bytes[length++] = 0x0A;
        }
        else if (app_equals_ignore_case(lhs, "A") && app_equals_ignore_case(rhs, "(DE)"))
        {
            bytes[length++] = 0x1A;
        }
        else if (dst8 >= 0 && src8 >= 0)
        {
            bytes[length++] = (uint8_t)(0x40 + (dst8 << 3) + src8);
        }
        else if (dst8 >= 0 && app_parse_value(ctx, rhs, &value))
        {
            if (ctx->pass == 2 && (value.value < -128 || value.value > 255))
            {
                snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
                return false;
            }
            bytes[length++] = (uint8_t)(0x06 + (dst8 << 3));
            bytes[length++] = (uint8_t)value.value;
        }
        else
        {
            snprintf(error_buffer, error_buffer_size, "Unsupported LD form: %s,%s", lhs, rhs);
            return false;
        }
    }
    else
    {
        snprintf(error_buffer, error_buffer_size, "Unsupported mnemonic: %s", mnemonic);
        return false;
    }

    if (!app_assemble_write_bytes(ctx, bytes, length, error_buffer, error_buffer_size))
    {
        return false;
    }
    ctx->total_written += length;
    return true;
}

/* Assembles a block of source text line by line and reports the first error
   with its line number so the small UI remains practical to use. */
static bool app_assemble_source(
    AppState *app,
    uint16_t start_address,
    const AssemblerPreparedSource *source,
    bool write_to_machine,
    AssemblerBinaryOutput *output,
    char *status_buffer,
    size_t status_buffer_size)
{
    AssemblerContext ctx;
    char *mutable_source;
    char *line;
    char *cursor;
    size_t line_number;
    bool success;

    memset(&ctx, 0, sizeof(ctx));
    ctx.app = app;
    ctx.start_address = start_address;
    ctx.address = start_address;
    ctx.pass = 1;
    ctx.write_to_machine = false;
    ctx.output = NULL;

    mutable_source = _strdup(source->text);
    if (mutable_source == NULL)
    {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }

    success = true;
    line_number = 0;
    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0')
    {
        char *line_end = strchr(cursor, '\n');
        line = cursor;
        if (line_end != NULL)
        {
            *line_end = '\0';
            cursor = line_end + 1;
        }
        else
        {
            cursor = NULL;
        }
        size_t line_length = strlen(line);
        if (line_length > 0 && line[line_length - 1] == '\r')
        {
            line[line_length - 1] = '\0';
        }
        line_number++;
        if (!app_assemble_line(
                &ctx,
                line,
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                status_buffer,
                status_buffer_size))
        {
            char final_error[512];
            app_assembler_format_location_error(
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                line_number,
                status_buffer,
                final_error,
                sizeof(final_error));
            snprintf(status_buffer, status_buffer_size, "%s", final_error);
            success = false;
            break;
        }
    }
    free(mutable_source);

    if (!success)
    {
        return false;
    }

    ctx.address = start_address;
    ctx.total_written = 0;
    ctx.pass = 2;
    ctx.write_to_machine = write_to_machine;
    ctx.output = output;
    if (output != NULL)
    {
        free(output->bytes);
        memset(output, 0, sizeof(*output));
    }

    mutable_source = _strdup(source->text);
    if (mutable_source == NULL)
    {
        snprintf(status_buffer, status_buffer_size, "Out of memory.");
        return false;
    }

    line_number = 0;
    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0')
    {
        char *line_end = strchr(cursor, '\n');
        line = cursor;
        if (line_end != NULL)
        {
            *line_end = '\0';
            cursor = line_end + 1;
        }
        else
        {
            cursor = NULL;
        }
        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r')
            {
                line[line_length - 1] = '\0';
            }
        }
        line_number++;
        if (!app_assemble_line(
                &ctx,
                line,
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                status_buffer,
                status_buffer_size))
        {
            char final_error[512];
            app_assembler_format_location_error(
                line_number <= source->line_count ? &source->locations[line_number - 1] : NULL,
                line_number,
                status_buffer,
                final_error,
                sizeof(final_error));
            snprintf(status_buffer, status_buffer_size, "%s", final_error);
            free(mutable_source);
            return false;
        }
    }
    free(mutable_source);

    snprintf(
        status_buffer,
        status_buffer_size,
        "Assembled %zu byte%s with %zu label%s and %zu constant%s to RAM starting at %04Xh. Next address: %04Xh.",
        ctx.total_written,
        ctx.total_written == 1 ? "" : "s",
        ctx.label_count,
        ctx.label_count == 1 ? "" : "s",
        ctx.constant_count,
        ctx.constant_count == 1 ? "" : "s",
        start_address,
        ctx.address);
    return true;
}

/* Releases any heap storage captured from one assembly pass. */
static void app_assembler_free_binary_output(AssemblerBinaryOutput *output)
{
    if (output == NULL)
    {
        return;
    }
    free(output->bytes);
    memset(output, 0, sizeof(*output));
}

/* Builds a 10-character Spectrum tape header name from a destination path. */
static void app_assembler_build_tap_name(const char *path, char out_name[11])
{
    const char *base = path;
    const char *slash;
    const char *dot;

    memset(out_name, ' ', 10);
    out_name[10] = '\0';
    if (path == NULL || path[0] == '\0')
    {
        memcpy(out_name, "PROGRAM   ", 10);
        return;
    }

    slash = strrchr(path, '\\');
    if (slash != NULL && slash[1] != '\0')
    {
        base = slash + 1;
    }
    dot = strrchr(base, '.');
    if (dot == NULL)
    {
        dot = base + strlen(base);
    }

    for (size_t i = 0; i < 10 && base + i < dot && base[i] != '\0'; ++i)
    {
        unsigned char ch = (unsigned char)base[i];
        out_name[i] = (char)((ch >= 32 && ch <= 126) ? toupper(ch) : '_');
    }
}

/* Appends one Spectrum TAP block to an output file, including its length
   prefix and XOR checksum byte. */
static bool app_assembler_write_tap_block(
    FILE *file,
    const uint8_t *payload,
    size_t payload_length,
    char *error_buffer,
    size_t error_buffer_size,
    const char *path)
{
    uint8_t checksum = 0;
    uint16_t block_length;

    if (payload_length > 0xFFFFu)
    {
        snprintf(error_buffer, error_buffer_size, "TAP block is too large: %s", path);
        return false;
    }
    block_length = (uint16_t)payload_length;
    if (fputc(block_length & 0xFF, file) == EOF ||
        fputc((block_length >> 8) & 0xFF, file) == EOF)
    {
        snprintf(error_buffer, error_buffer_size, "Could not write TAP file: %s", path);
        return false;
    }
    for (size_t i = 0; i < payload_length; ++i)
    {
        checksum ^= payload[i];
        if (fputc(payload[i], file) == EOF)
        {
            snprintf(error_buffer, error_buffer_size, "Could not write TAP file: %s", path);
            return false;
        }
    }
    if (fputc(checksum, file) == EOF)
    {
        snprintf(error_buffer, error_buffer_size, "Could not write TAP file: %s", path);
        return false;
    }
    return true;
}

/* Writes one standard Spectrum CODE file as a two-block TAP image. */
static bool app_assembler_write_tap_file(
    const char *path,
    uint16_t load_address,
    const uint8_t *data,
    size_t length,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    uint8_t header[18];
    uint8_t *block_data;
    char tape_name[11];
    bool ok = false;

    if (length == 0 || length > 0xFFFFu)
    {
        snprintf(error_buffer, error_buffer_size, "TAP export requires 1-65535 assembled bytes.");
        return false;
    }

    app_assembler_build_tap_name(path, tape_name);
    memset(header, 0, sizeof(header));
    header[0] = 0x00;
    header[1] = 0x03;
    memcpy(&header[2], tape_name, 10);
    header[12] = (uint8_t)(length & 0xFF);
    header[13] = (uint8_t)((length >> 8) & 0xFF);
    header[14] = (uint8_t)(load_address & 0xFF);
    header[15] = (uint8_t)((load_address >> 8) & 0xFF);
    header[16] = 0x00;
    header[17] = 0x80;

    block_data = (uint8_t *)malloc(length + 1);
    if (block_data == NULL)
    {
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }
    block_data[0] = 0xFF;
    memcpy(block_data + 1, data, length);

    file = fopen(path, "wb");
    if (file == NULL)
    {
        free(block_data);
        snprintf(error_buffer, error_buffer_size, "Could not write TAP file: %s", path);
        return false;
    }

    if (app_assembler_write_tap_block(file, header, sizeof(header), error_buffer, error_buffer_size, path) &&
        app_assembler_write_tap_block(file, block_data, length + 1, error_buffer, error_buffer_size, path))
    {
        ok = true;
    }

    fclose(file);
    free(block_data);

    if (!ok)
    {
        remove(path);
        return false;
    }
    return true;
}

/* Seeds the assembler with the current PC and a brief syntax reminder. */

static bool app_assembler_format_source_text(const char *source, char **formatted_output)
{
    size_t source_length;
    size_t capacity;
    size_t used = 0;
    char *formatted;
    const char *cursor;

    if (source == NULL || formatted_output == NULL)
    {
        return false;
    }

    source_length = strlen(source);
    capacity = source_length + (source_length / 2) + 64;
    formatted = (char *)malloc(capacity);
    if (formatted == NULL)
    {
        return false;
    }

    cursor = source;
    while (*cursor != '\0')
    {
        const char *line_start = cursor;
        const char *line_end = cursor;
        const char *trimmed_start;
        const char *trimmed_end;
        const char *body_start;
        const char *body_end;
        const char *label_end;
        const char *next_cursor;
        bool has_crlf = false;
        bool is_blank;
        bool has_label = false;
        char label[64];

        while (*line_end != '\0' && *line_end != '\r' && *line_end != '\n')
        {
            line_end++;
        }
        next_cursor = line_end;
        if (*next_cursor == '\r')
        {
            has_crlf = true;
            next_cursor++;
        }
        if (*next_cursor == '\n')
        {
            has_crlf = true;
            next_cursor++;
        }

        trimmed_start = line_start;
        while (trimmed_start < line_end && isspace((unsigned char)*trimmed_start))
        {
            trimmed_start++;
        }
        trimmed_end = line_end;
        while (trimmed_end > trimmed_start && isspace((unsigned char)trimmed_end[-1]))
        {
            trimmed_end--;
        }
        is_blank = trimmed_start == trimmed_end;

        if (!is_blank)
        {
            body_start = trimmed_start;
            body_end = trimmed_end;
            label_end = trimmed_start;
            while (app_parse_leading_label((char **)&body_start, label, sizeof(label)))
            {
                has_label = true;
                label_end = body_start;
                while (body_start < body_end && isspace((unsigned char)*body_start))
                {
                    body_start++;
                }
                if (body_start >= body_end)
                {
                    break;
                }
            }

            if (has_label)
            {
                size_t line_length = (size_t)(trimmed_end - trimmed_start);
                if (used + line_length + 7 >= capacity)
                {
                    capacity = capacity + line_length + 256;
                    formatted = (char *)realloc(formatted, capacity);
                    if (formatted == NULL)
                    {
                        return false;
                    }
                }
                memcpy(formatted + used, trimmed_start, (size_t)(label_end - trimmed_start));
                used += (size_t)(label_end - trimmed_start);
                if (body_start < body_end)
                {
                    const char *comment_start = app_find_comment_start(body_start, body_end);
                    const char *code_end = comment_start != NULL ? comment_start : body_end;

                    while (code_end > body_start && isspace((unsigned char)code_end[-1]))
                    {
                        code_end--;
                    }
                    formatted[used++] = '\r';
                    formatted[used++] = '\n';
                    if (comment_start == NULL || comment_start > body_start)
                    {
                        formatted[used++] = '\t';
                    }
                    if (code_end > body_start)
                    {
                        memcpy(formatted + used, body_start, (size_t)(code_end - body_start));
                        used += (size_t)(code_end - body_start);
                    }
                    if (comment_start != NULL)
                    {
                        if (code_end > body_start)
                        {
                            formatted[used++] = '\t';
                        }
                        if (!app_append_formatted_comment(&formatted, &capacity, &used, comment_start, body_end))
                        {
                            free(formatted);
                            return false;
                        }
                    }
                }
            }
            else
            {
                size_t line_length = (size_t)(trimmed_end - trimmed_start);
                const char *comment_start = app_find_comment_start(trimmed_start, trimmed_end);
                const char *code_end = comment_start != NULL ? comment_start : trimmed_end;

                while (code_end > trimmed_start && isspace((unsigned char)code_end[-1]))
                {
                    code_end--;
                }
                if (used + line_length + 5 >= capacity)
                {
                    capacity = capacity + line_length + 256;
                    formatted = (char *)realloc(formatted, capacity);
                    if (formatted == NULL)
                    {
                        return false;
                    }
                }
                if (comment_start == NULL || comment_start > trimmed_start)
                {
                    formatted[used++] = '\t';
                }
                if (code_end > trimmed_start)
                {
                    memcpy(formatted + used, trimmed_start, (size_t)(code_end - trimmed_start));
                    used += (size_t)(code_end - trimmed_start);
                }
                if (comment_start != NULL)
                {
                    if (code_end > trimmed_start)
                    {
                        formatted[used++] = '\t';
                    }
                    if (!app_append_formatted_comment(&formatted, &capacity, &used, comment_start, trimmed_end))
                    {
                        free(formatted);
                        return false;
                    }
                }
            }
        }

        if (has_crlf)
        {
            if (used + 2 >= capacity)
            {
                capacity += 256;
                formatted = (char *)realloc(formatted, capacity);
                if (formatted == NULL)
                {
                    return false;
                }
            }
            formatted[used++] = '\r';
            formatted[used++] = '\n';
        }

        cursor = next_cursor;
    }

    if (used + 1 >= capacity)
    {
        capacity += 1;
        formatted = (char *)realloc(formatted, capacity);
        if (formatted == NULL)
        {
            return false;
        }
    }
    formatted[used] = '\0';
    *formatted_output = formatted;
    return true;
}

static bool app_assembler_uppercase_source_text(const char *source, char **uppercase_output)
{
    size_t source_length;
    char *uppercase_text;
    bool in_single = false;
    bool in_double = false;
    bool in_comment = false;

    if (source == NULL || uppercase_output == NULL)
    {
        return false;
    }

    source_length = strlen(source);
    uppercase_text = (char *)malloc(source_length + 1);
    if (uppercase_text == NULL)
    {
        return false;
    }

    for (size_t i = 0; i < source_length; ++i)
    {
        const char ch = source[i];

        if (in_comment)
        {
            uppercase_text[i] = ch;
        }
        else if (ch == '\'' && !in_double)
        {
            in_single = !in_single;
            uppercase_text[i] = ch;
        }
        else if (ch == '"' && !in_single)
        {
            in_double = !in_double;
            uppercase_text[i] = ch;
        }
        else if (ch == ';' && !in_single && !in_double)
        {
            in_comment = true;
            uppercase_text[i] = ch;
        }
        else if (!in_single && !in_double)
        {
            uppercase_text[i] = (char)toupper((unsigned char)ch);
        }
        else
        {
            uppercase_text[i] = ch;
        }

        if (ch == '\n' || ch == '\r')
        {
            in_comment = false;
        }
    }

    uppercase_text[source_length] = '\0';
    *uppercase_output = uppercase_text;
    return true;
}

static bool app_file_exists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Joins two path fragments with a single backslash and reports whether the
   resulting string fit inside the caller-provided output buffer. */
static bool app_join_path(char *out, size_t out_size, const char *left, const char *right)
{
    int written = snprintf(out, out_size, "%s\\%s", left, right);
    return written > 0 && (size_t)written < out_size;
}

/* Mutates a path string in place so it becomes its parent directory and
   returns false if there is no directory separator to trim. */
static bool app_parent_dir(char *path)
{
    char *slash = strrchr(path, '\\');
    if (slash == NULL)
    {
        return false;
    }
    *slash = '\0';
    return true;
}

/* Releases any heap storage attached to a prepared assembler source bundle. */
static void app_assembler_free_prepared_source(AssemblerPreparedSource *prepared)
{
    if (prepared == NULL)
    {
        return;
    }
    free(prepared->text);
    free(prepared->locations);
    memset(prepared, 0, sizeof(*prepared));
}

/* Returns true when the supplied path is already absolute on Windows. */
static bool app_path_is_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }
    if (path[0] == '\\' || path[0] == '/')
    {
        return true;
    }
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

/* Converts a raw file operand into a plain path, accepting either quoted or
   unquoted forms so directives such as `INCLUDE file.asm` and
   `INCBIN "data.bin"` both work. */
static bool app_assembler_extract_path_operand(
    const char *directive_name,
    const char *operand,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    char token[512];
    char *trimmed;
    size_t length;

    snprintf(token, sizeof(token), "%s", operand);
    trimmed = app_trim_inplace(token);
    if (trimmed != token)
    {
        memmove(token, trimmed, strlen(trimmed) + 1);
    }
    length = strlen(token);
    if (length == 0)
    {
        snprintf(error_buffer, error_buffer_size, "%s expects a file path.", directive_name);
        return false;
    }
    if ((token[0] == '"' || token[0] == '\'') && length >= 2 && token[length - 1] == token[0])
    {
        token[length - 1] = '\0';
        memmove(token, token + 1, strlen(token));
    }
    if (token[0] == '\0')
    {
        snprintf(error_buffer, error_buffer_size, "%s expects a file path.", directive_name);
        return false;
    }
    snprintf(out_path, out_size, "%s", token);
    return true;
}

/* Resolves a directive file path relative to the file that referenced it when
   one is available, otherwise against the current process working directory. */
static bool app_assembler_resolve_source_path(
    const char *including_path,
    const char *operand,
    const char *directive_name,
    char *out_path,
    size_t out_size,
    char *error_buffer,
    size_t error_buffer_size)
{
    char relative_path[MAX_PATH];
    char candidate[MAX_PATH];
    DWORD resolved_length;

    if (!app_assembler_extract_path_operand(
            directive_name,
            operand,
            relative_path,
            sizeof(relative_path),
            error_buffer,
            error_buffer_size))
    {
        return false;
    }

    if (app_path_is_absolute(relative_path))
    {
        snprintf(candidate, sizeof(candidate), "%s", relative_path);
    }
    else if (including_path != NULL && including_path[0] != '\0')
    {
        char parent[MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", including_path);
        if (!app_parent_dir(parent) || !app_join_path(candidate, sizeof(candidate), parent, relative_path))
        {
            snprintf(error_buffer, error_buffer_size, "%s path is too long: %s", directive_name, relative_path);
            return false;
        }
    }
    else
    {
        snprintf(candidate, sizeof(candidate), "%s", relative_path);
    }

    resolved_length = GetFullPathNameA(candidate, (DWORD)out_size, out_path, NULL);
    if (resolved_length == 0 || resolved_length >= out_size)
    {
        snprintf(error_buffer, error_buffer_size, "Could not resolve %s path: %s", directive_name, relative_path);
        return false;
    }
    if (!app_file_exists(out_path))
    {
        snprintf(error_buffer, error_buffer_size, "%s file not found: %s", directive_name, out_path);
        return false;
    }
    return true;
}

/* Reads a text file and normalizes all line endings to bare LF characters so
   assembler line handling works consistently across source files. */
static bool app_read_text_file_normalized(
    const char *path,
    char **out_text,
    char *error_buffer,
    size_t error_buffer_size)
{
    FILE *file;
    long file_size;
    char *raw_data;
    char *normalized;
    size_t normalized_length = 0;

    *out_text = NULL;
    file = fopen(path, "rb");
    if (file == NULL)
    {
        snprintf(error_buffer, error_buffer_size, "Could not open include file: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    file_size = ftell(file);
    if (file_size < 0)
    {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    rewind(file);

    raw_data = (char *)malloc((size_t)file_size + 1);
    if (raw_data == NULL)
    {
        fclose(file);
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }
    if (file_size > 0 && fread(raw_data, 1, (size_t)file_size, file) != (size_t)file_size)
    {
        fclose(file);
        free(raw_data);
        snprintf(error_buffer, error_buffer_size, "Could not read include file: %s", path);
        return false;
    }
    fclose(file);
    raw_data[file_size] = '\0';

    normalized = (char *)malloc((size_t)file_size + 1);
    if (normalized == NULL)
    {
        free(raw_data);
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }

    for (long i = 0; i < file_size; ++i)
    {
        unsigned char ch = (unsigned char)raw_data[i];
        if (ch == '\r')
        {
            if (i + 1 < file_size && raw_data[i + 1] == '\n')
            {
                i++;
            }
            normalized[normalized_length++] = '\n';
        }
        else
        {
            normalized[normalized_length++] = (char)ch;
        }
    }
    normalized[normalized_length] = '\0';

    if (normalized_length >= 3 &&
        (unsigned char)normalized[0] == 0xEF &&
        (unsigned char)normalized[1] == 0xBB &&
        (unsigned char)normalized[2] == 0xBF)
    {
        memmove(normalized, normalized + 3, normalized_length - 2);
    }

    free(raw_data);
    *out_text = normalized;
    return true;
}

/* Extends the prepared-source buffers and records one logical source line plus
   its originating file and line number. */
static bool app_assembler_append_prepared_line(
    AssemblerPreparedSource *prepared,
    const char *line,
    const char *source_path,
    size_t source_line,
    char *error_buffer,
    size_t error_buffer_size)
{
    size_t line_length = strlen(line);

    if (prepared->text_length + line_length + 2 > prepared->text_capacity)
    {
        size_t new_capacity = prepared->text_capacity == 0 ? 1024 : prepared->text_capacity * 2;
        while (new_capacity < prepared->text_length + line_length + 2)
        {
            new_capacity *= 2;
        }
        char *new_text = (char *)realloc(prepared->text, new_capacity);
        if (new_text == NULL)
        {
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }
        prepared->text = new_text;
        prepared->text_capacity = new_capacity;
    }
    if (prepared->line_count + 1 > prepared->line_capacity)
    {
        size_t new_capacity = prepared->line_capacity == 0 ? 64 : prepared->line_capacity * 2;
        AssemblerSourceLocation *new_locations =
            (AssemblerSourceLocation *)realloc(prepared->locations, new_capacity * sizeof(AssemblerSourceLocation));
        if (new_locations == NULL)
        {
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }
        prepared->locations = new_locations;
        prepared->line_capacity = new_capacity;
    }

    memcpy(prepared->text + prepared->text_length, line, line_length);
    prepared->text_length += line_length;
    prepared->text[prepared->text_length++] = '\n';
    prepared->text[prepared->text_length] = '\0';

    prepared->locations[prepared->line_count].path[0] = '\0';
    if (source_path != NULL)
    {
        snprintf(prepared->locations[prepared->line_count].path, MAX_PATH, "%s", source_path);
    }
    prepared->locations[prepared->line_count].line_number = source_line;
    prepared->line_count++;
    return true;
}

/* Formats assembler diagnostics with file-and-line context when expanded
   source originated from one or more INCLUDEd files. */
static void app_assembler_format_location_error(
    const AssemblerSourceLocation *location,
    size_t fallback_line,
    const char *message,
    char *out_error,
    size_t out_error_size)
{
    size_t line_number = fallback_line;

    if (location != NULL && location->line_number != 0)
    {
        line_number = location->line_number;
    }
    if (location != NULL && location->path[0] != '\0')
    {
        snprintf(out_error, out_error_size, "%s line %zu: %s", location->path, line_number, message);
    }
    else
    {
        snprintf(out_error, out_error_size, "Line %zu: %s", line_number, message);
    }
}

/* Recursively expands INCLUDE directives into a flat source buffer while
   preserving source locations for later error reporting. */
static bool app_assembler_expand_source(
    const char *source,
    const char *source_path,
    AssemblerPreparedSource *prepared,
    char include_stack[][MAX_PATH],
    size_t include_depth,
    char *error_buffer,
    size_t error_buffer_size)
{
    char *mutable_source;
    char *cursor;
    size_t line_number = 0;

    if (include_depth >= 32)
    {
        snprintf(error_buffer, error_buffer_size, "INCLUDE nesting is too deep.");
        return false;
    }

    mutable_source = _strdup(source);
    if (mutable_source == NULL)
    {
        snprintf(error_buffer, error_buffer_size, "Out of memory.");
        return false;
    }

    cursor = mutable_source;
    while (cursor != NULL && *cursor != '\0')
    {
        char *line_end = strchr(cursor, '\n');
        char *line = cursor;
        char *analysis_line;

        if (line_end != NULL)
        {
            *line_end = '\0';
            cursor = line_end + 1;
        }
        else
        {
            cursor = NULL;
        }

        {
            size_t line_length = strlen(line);
            if (line_length > 0 && line[line_length - 1] == '\r')
            {
                line[line_length - 1] = '\0';
            }
        }
        line_number++;

        analysis_line = _strdup(line);
        if (analysis_line == NULL)
        {
            free(mutable_source);
            snprintf(error_buffer, error_buffer_size, "Out of memory.");
            return false;
        }

        app_strip_comment(analysis_line);
        {
            char mnemonic[32];
            char lhs[256];
            char rhs[256];
            char label[64];
            char *text = app_trim_inplace(analysis_line);
            size_t mnemonic_chars;
            int operand_result;
            int operand_count = 0;

            while (app_parse_leading_label(&text, label, sizeof(label)))
            {
                text = app_trim_inplace(text);
                if (*text == '\0')
                {
                    break;
                }
            }

            if (*text != '\0')
            {
                mnemonic_chars = app_read_upper_token(text, mnemonic, sizeof(mnemonic));
                text += mnemonic_chars;
                text = app_trim_inplace(text);
                lhs[0] = '\0';
                rhs[0] = '\0';

                if (*text != '\0')
                {
                    char *operand_cursor = text;
                    operand_result = app_read_operand_strict(&operand_cursor, lhs, sizeof(lhs), error_buffer, error_buffer_size);
                    if (operand_result < 0)
                    {
                        AssemblerSourceLocation location = {{0}, line_number};
                        char final_error[512];
                        if (source_path != NULL)
                        {
                            snprintf(location.path, sizeof(location.path), "%s", source_path);
                        }
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (operand_result > 0)
                    {
                        operand_count = 1;
                        operand_result = app_read_operand_strict(&operand_cursor, rhs, sizeof(rhs), error_buffer, error_buffer_size);
                        if (operand_result < 0)
                        {
                            AssemblerSourceLocation location = {{0}, line_number};
                            char final_error[512];
                            if (source_path != NULL)
                            {
                                snprintf(location.path, sizeof(location.path), "%s", source_path);
                            }
                            app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                            snprintf(error_buffer, error_buffer_size, "%s", final_error);
                            free(analysis_line);
                            free(mutable_source);
                            return false;
                        }
                        if (operand_result > 0)
                        {
                            operand_count = 2;
                        }
                    }
                }

                if (app_equals_ignore_case(mnemonic, "INCLUDE"))
                {
                    char include_path[MAX_PATH];
                    char *include_source;
                    AssemblerSourceLocation location = {{0}, line_number};

                    if (source_path != NULL)
                    {
                        snprintf(location.path, sizeof(location.path), "%s", source_path);
                    }
                    if (operand_count != 1)
                    {
                        char final_error[512];
                        app_assembler_format_location_error(
                            &location,
                            line_number,
                            "INCLUDE expects 1 operand.",
                            final_error,
                            sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (!app_assembler_resolve_source_path(
                            source_path,
                            lhs,
                            "INCLUDE",
                            include_path,
                            sizeof(include_path),
                            error_buffer,
                            error_buffer_size))
                    {
                        char final_error[512];
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    for (size_t i = 0; i < include_depth; ++i)
                    {
                        if (_stricmp(include_stack[i], include_path) == 0)
                        {
                            char circular_error[512];
                            snprintf(circular_error, sizeof(circular_error), "Circular INCLUDE detected: %s", include_path);
                            app_assembler_format_location_error(
                                &location,
                                line_number,
                                circular_error,
                                error_buffer,
                                error_buffer_size);
                            free(analysis_line);
                            free(mutable_source);
                            return false;
                        }
                    }
                    snprintf(include_stack[include_depth], MAX_PATH, "%s", include_path);
                    if (!app_read_text_file_normalized(include_path, &include_source, error_buffer, error_buffer_size))
                    {
                        char final_error[512];
                        app_assembler_format_location_error(&location, line_number, error_buffer, final_error, sizeof(final_error));
                        snprintf(error_buffer, error_buffer_size, "%s", final_error);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    if (!app_assembler_expand_source(
                            include_source,
                            include_path,
                            prepared,
                            include_stack,
                            include_depth + 1,
                            error_buffer,
                            error_buffer_size))
                    {
                        free(include_source);
                        free(analysis_line);
                        free(mutable_source);
                        return false;
                    }
                    free(include_source);
                    free(analysis_line);
                    continue;
                }
            }
        }

        free(analysis_line);
        if (!app_assembler_append_prepared_line(
                prepared,
                line,
                source_path,
                line_number,
                error_buffer,
                error_buffer_size))
        {
            free(mutable_source);
            return false;
        }
    }

    free(mutable_source);
    return true;
}

/* Builds the final source buffer used by ORG scanning and the two assembly
   passes, expanding any nested INCLUDE directives first. */
static bool app_assembler_prepare_source(
    AppState *app,
    const char *source,
    AssemblerPreparedSource *prepared,
    char *status_buffer,
    size_t status_buffer_size)
{
    char include_stack[32][MAX_PATH];
    const char *root_path = NULL;
    size_t include_depth = 0;

    memset(prepared, 0, sizeof(*prepared));
    if (app != NULL && app->debug.assembler_current_path[0] != '\0')
    {
        root_path = app->debug.assembler_current_path;
        snprintf(include_stack[0], MAX_PATH, "%s", root_path);
        include_depth = 1;
    }
    if (!app_assembler_expand_source(
            source,
            root_path,
            prepared,
            include_stack,
            include_depth,
            status_buffer,
            status_buffer_size))
    {
        app_assembler_free_prepared_source(prepared);
        return false;
    }
    if (prepared->text == NULL)
    {
        prepared->text = _strdup("");
        if (prepared->text == NULL)
        {
            snprintf(status_buffer, status_buffer_size, "Out of memory.");
            return false;
        }
        prepared->text_capacity = 1;
    }
    return true;
}

/* Searches for a ROM file in the current working directory, next to the EXE,
   and then one directory above the EXE to support the repo's layout. */