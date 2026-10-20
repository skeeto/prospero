// JSON tests: a JSONTestSuite subset (github.com/nst/JSONTestSuite),
// canonical round trips, accessors, escaping, numbers, and nesting

// Parse and write back canonically, or describe the error.
static Str jsoncanon(Arena *a, Str in)
{
    JsonDoc d = jsonparse(a, in);
    Buf     b(a, 64);
    if (!d.ok) {
        print(&b, "error at ");
        print(&b, (i64)d.off);
        print(&b, ": ");
        print(&b, d.err);
        return finish(&b);
    }
    JsonOut w = {&b};
    putjson(&w, d.root);
    return finish(&b);
}

static Str jsonverdict(Arena *a, Str in)
{
    return jsonparse(a, in).ok ? Str("accept") : Str("reject");
}

// Describe a node for accessor tests.
static Str jsonshow(Arena *a, Json *j)
{
    static Str types[] = {
        "missing", "null", "bool", "number", "string", "array", "object"
    };
    Buf b(a, 64);
    print(&b, types[j->type]);
    if (j->type == JSON_STRING) {
        print(&b, " ");
        print(&b, j->str);
    } else if (j->type==JSON_NUMBER || j->type==JSON_BOOL) {
        print(&b, " ");
        print(&b, j->raw);
    } else if (j->type==JSON_ARRAY || j->type==JSON_OBJECT) {
        print(&b, " ");
        print(&b, (i64)jsonlen(j));
    }
    return finish(&b);
}

static Str fnum(Arena *a, f64 v)
{
    Buf     b(a, 32);
    JsonOut w = {&b};
    putfloat(&w, v);
    return finish(&b);
}

// Normalize a decimal to its significant digits and the exponent of the
// first one: "1.50e3", "1500", and "0.0015e6" are all "15e3".
static Str decnorm(Arena *a, Str s)
{
    Cut c = cut(s, 'e');
    i64 e = c.ok ? parseint(c.tail).val : 0;
    Buf d(a, 32);
    iz  point = -1;
    for (iz i = 0; i < c.head.len; i++) {
        if (c.head[i] == '.') {
            point = d.len;
        } else if (digit(c.head[i])) {
            putbyte(&d, c.head[i]);
        }
    }
    point = point<0 ? d.len : point;
    Str digits = finish(&d);
    iz  lead   = 0;
    for (; lead<digits.len && digits[lead]=='0'; lead++) {}
    digits = cuthead(digits, lead);
    for (; digits.len && digits[digits.len-1]=='0'; digits.len--) {}
    if (!digits.len) {
        return "0";
    }
    Buf b(a, 32);
    print(&b, digits);
    putbyte(&b, 'e');
    print(&b, e + point - lead - 1);
    return finish(&b);
}

// The integer value of a number, or "default".
static Str jsonintstr(Arena *a, Str in)
{
    Json *j  = jsonparse(a, in).root;
    i64   r1 = jsonint(j, 1);
    i64   r2 = jsonint(j, 2);
    return r1==r2 ? istr(a, r1) : Str("default");
}

// Write a parsed document as the "v" member of an object, as a request
// or record would carry it.
static Str jsonembed(Arena *a, Str in)
{
    JsonDoc d = jsonparse(a, in);
    Buf     b(a, 256);
    JsonOut w = {&b};
    putobject(&w);
    putkey(&w, "v");
    putjson(&w, d.root);
    putend(&w);
    return finish(&b);
}

static Str nested(Arena *a, i32 depth, Str open, Str close)
{
    Buf b(a, 256);
    for (i32 i = 0; i < depth; i++) print(&b, open);
    for (i32 i = 0; i < depth; i++) print(&b, close);
    return finish(&b);
}

// Objects nested levels deep: {"a":{"a":{}}} is three.
static Str nestedobj(Arena *a, i32 levels)
{
    Buf b(a, 256);
    for (i32 i = 1; i < levels; i++) print(&b, "{\"a\":");
    print(&b, "{}");
    for (i32 i = 1; i < levels; i++) putbyte(&b, '}');
    return finish(&b);
}

static void test_json(Test *t)
{
    Arena a = t->perm;

    // Must accept: each also round-trips canonically (write, reparse,
    // write again gives identical bytes).
    struct { Str name, in; } yes[] = {
        {"y_array_arraysWithSpaces", "[[]   ]"},
        {"y_array_empty", "[]"},
        {"y_array_empty-string", "[\"\"]"},
        {"y_array_ending_with_newline", "[\"a\"]"},
        {"y_array_false", "[false]"},
        {"y_array_heterogeneous", "[null, 1, \"1\", {}]"},
        {"y_array_null", "[null]"},
        {"y_array_with_1_and_newline", "[1\n]"},
        {"y_array_with_leading_space", " [1]"},
        {"y_array_with_several_null", "[1,null,null,null,2]"},
        {"y_array_with_trailing_space", "[2] "},
        {"y_number", "[123e65]"},
        {"y_number_0e+1", "[0e+1]"},
        {"y_number_0e1", "[0e1]"},
        {"y_number_after_space", "[ 4]"},
        {"y_number_double_close_to_zero", "[-0.0000000000000000000000000000000000000000000000000000000000000000000000000000001]"},
        {"y_number_int_with_exp", "[20e1]"},
        {"y_number_minus_zero", "[-0]"},
        {"y_number_negative_int", "[-123]"},
        {"y_number_negative_one", "[-1]"},
        {"y_number_real_capital_e", "[1E22]"},
        {"y_number_real_capital_e_neg_exp", "[1E-2]"},
        {"y_number_real_capital_e_pos_exp", "[1E+2]"},
        {"y_number_real_exponent", "[123e45]"},
        {"y_number_real_fraction_exponent", "[123.456e78]"},
        {"y_number_real_neg_exp", "[1e-2]"},
        {"y_number_real_pos_exponent", "[1e+2]"},
        {"y_number_simple_int", "[123]"},
        {"y_number_simple_real", "[123.456789]"},
        {"y_object", "{\"asd\":\"sdf\", \"dfg\":\"fgh\"}"},
        {"y_object_basic", "{\"asd\":\"sdf\"}"},
        {"y_object_duplicated_key", "{\"a\":\"b\",\"a\":\"c\"}"},
        {"y_object_duplicated_key_and_value", "{\"a\":\"b\",\"a\":\"b\"}"},
        {"y_object_empty", "{}"},
        {"y_object_empty_key", "{\"\":0}"},
        {"y_object_escaped_null_in_key", "{\"foo\\u0000bar\": 42}"},
        {"y_object_extreme_numbers", "{ \"min\": -1.0e+28, \"max\": 1.0e+28 }"},
        {"y_object_long_strings", "{\"x\":[{\"id\": \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}], \"id\": \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}"},
        {"y_object_simple", "{\"a\":[]}"},
        {"y_object_string_unicode", "{\"title\":\"\\u041f\\u043e\\u043b\\u0442\\u043e\\u0440\\u0430 \\u0417\\u0435\\u043c\\u043b\\u0435\\u043a\\u043e\\u043f\\u0430\" }"},
        {"y_object_with_newlines", "{\n\"a\": \"b\"\n}"},
        {"y_string_1_2_3_bytes_UTF-8_sequences", "[\"\\u0060\\u012a\\u12AB\"]"},
        {"y_string_accepted_surrogate_pair", "[\"\\uD801\\udc37\"]"},
        {"y_string_accepted_surrogate_pairs", "[\"\\ud83d\\ude39\\ud83d\\udc8d\"]"},
        {"y_string_allowed_escapes", "[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]"},
        {"y_string_backslash_and_u_escaped_zero", "[\"\\\\u0000\"]"},
        {"y_string_backslash_doublequotes", "[\"\\\"\"]"},
        {"y_string_comments", "[\"a/*b*/c/*d//e\"]"},
        {"y_string_double_escape_a", "[\"\\\\a\"]"},
        {"y_string_double_escape_n", "[\"\\\\n\"]"},
        {"y_string_escaped_control_character", "[\"\\u0012\"]"},
        {"y_string_escaped_noncharacter", "[\"\\uFFFF\"]"},
        {"y_string_in_array", "[\"asd\"]"},
        {"y_string_in_array_with_leading_space", "[ \"asd\"]"},
        {"y_string_last_surrogates_1_and_2", "[\"\\uDBFF\\uDFFF\"]"},
        {"y_string_nbsp_uescaped", "[\"new\\u00A0line\"]"},
        {"y_string_nonCharacterInUTF-8_U+10FFFF", "[\"\xf4\x8f\xbf\xbf\"]"},
        {"y_string_nonCharacterInUTF-8_U+FFFF", "[\"\xef\xbf\xbf\"]"},
        {"y_string_null_escape", "[\"\\u0000\"]"},
        {"y_string_one-byte-utf-8", "[\"\\u002c\"]"},
        {"y_string_pi", "[\"\xcf\x80\"]"},
        {"y_string_reservedCharacterInUTF-8_U+1BFFF", "[\"\xf0\x9b\xbf\xbf\"]"},
        {"y_string_simple_ascii", "[\"asd \"]"},
        {"y_string_space", "\" \""},
        {"y_string_surrogates_U+1D11E_MUSICAL_SYMBOL_G_CLEF", "[\"\\uD834\\uDd1e\"]"},
        {"y_string_three-byte-utf-8", "[\"\\u0821\"]"},
        {"y_string_two-byte-utf-8", "[\"\\u0123\"]"},
        {"y_string_u+2028_line_sep", "[\"\xe2\x80\xa8\"]"},
        {"y_string_u+2029_par_sep", "[\"\xe2\x80\xa9\"]"},
        {"y_string_uEscape", "[\"\\u0061\\u30af\\u30EA\\u30b9\"]"},
        {"y_string_uescaped_newline", "[\"new\\u000Aline\"]"},
        {"y_string_unescaped_char_delete", "[\"\x7f\"]"},
        {"y_string_unicode", "[\"\\uA66D\"]"},
        {"y_string_unicodeEscapedBackslash", "[\"\\u005C\"]"},
        {"y_string_unicode_2", "[\"\xe2\x8d\x82\xe3\x88\xb4\xe2\x8d\x82\"]"},
        {"y_string_unicode_U+10FFFE_nonchar", "[\"\\uDBFF\\uDFFE\"]"},
        {"y_string_unicode_U+1FFFE_nonchar", "[\"\\uD83F\\uDFFE\"]"},
        {"y_string_unicode_U+200B_ZERO_WIDTH_SPACE", "[\"\\u200B\"]"},
        {"y_string_unicode_U+2064_invisible_plus", "[\"\\u2064\"]"},
        {"y_string_unicode_U+FDD0_nonchar", "[\"\\uFDD0\"]"},
        {"y_string_unicode_U+FFFE_nonchar", "[\"\\uFFFE\"]"},
        {"y_string_unicode_escaped_double_quote", "[\"\\u0022\"]"},
        {"y_string_utf8", "[\"\xe2\x82\xac\xf0\x9d\x84\x9e\"]"},
        {"y_string_with_del_character", "[\"a\x7f" "a\"]"},
        {"y_structure_lonely_false", "false"},
        {"y_structure_lonely_int", "42"},
        {"y_structure_lonely_negative_real", "-0.1"},
        {"y_structure_lonely_null", "null"},
        {"y_structure_lonely_string", "\"asd\""},
        {"y_structure_lonely_true", "true"},
        {"y_structure_string_empty", "\"\""},
        {"y_structure_trailing_newline", "[\"a\"]\n"},
        {"y_structure_true_in_array", "[true]"},
        {"y_structure_whitespace_array", " [] "},
    };
    for (iz i = 0; i < countof(yes); i++) {
        Str once = jsoncanon(&a, yes[i].in);
        expect(t, yes[i].name, jsonverdict(&a, yes[i].in), "accept");
        expect(t, yes[i].name, jsoncanon(&a, once), once);
    }

    // Must reject
    struct { Str name, in; } no[] = {
        {"n_array_1_true_without_comma", "[1 true]"},
        {"n_array_a_invalid_utf8", "[a\xe5]"},
        {"n_array_colon_instead_of_comma", "[\"\": 1]"},
        {"n_array_comma_after_close", "[\"\"],"},
        {"n_array_comma_and_number", "[,1]"},
        {"n_array_double_comma", "[1,,2]"},
        {"n_array_double_extra_comma", "[\"x\",,]"},
        {"n_array_extra_close", "[\"x\"]]"},
        {"n_array_extra_comma", "[\"\",]"},
        {"n_array_incomplete", "[\"x\""},
        {"n_array_incomplete_invalid_value", "[x"},
        {"n_array_inner_array_no_comma", "[3[4]]"},
        {"n_array_invalid_utf8", "[\xff]"},
        {"n_array_items_separated_by_semicolon", "[1:2]"},
        {"n_array_just_comma", "[,]"},
        {"n_array_just_minus", "[-]"},
        {"n_array_missing_value", "[   , \"\"]"},
        {"n_array_newlines_unclosed", "[\"a\",\n4\n,1,"},
        {"n_array_number_and_comma", "[1,]"},
        {"n_array_number_and_several_commas", "[1,,]"},
        {"n_array_spaces_vertical_tab_formfeed", "[\"\va\"\\f]"},
        {"n_array_star_inside", "[*]"},
        {"n_array_unclosed", "[\"\""},
        {"n_array_unclosed_trailing_comma", "[1,"},
        {"n_array_unclosed_with_new_lines", "[1,\n1\n,1"},
        {"n_array_unclosed_with_object_inside", "[{}"},
        {"n_incomplete_false", "[fals]"},
        {"n_incomplete_null", "[nul]"},
        {"n_incomplete_true", "[tru]"},
        {"n_multidigit_number_then_00", "123\x00"},
        {"n_number_++", "[++1234]"},
        {"n_number_+1", "[+1]"},
        {"n_number_+Inf", "[+Inf]"},
        {"n_number_-01", "[-01]"},
        {"n_number_-1.0.", "[-1.0.]"},
        {"n_number_-2.", "[-2.]"},
        {"n_number_-NaN", "[-NaN]"},
        {"n_number_.-1", "[.-1]"},
        {"n_number_.2e-3", "[.2e-3]"},
        {"n_number_0.1.2", "[0.1.2]"},
        {"n_number_0.3e+", "[0.3e+]"},
        {"n_number_0.3e", "[0.3e]"},
        {"n_number_0.e1", "[0.e1]"},
        {"n_number_0_capital_E+", "[0E+]"},
        {"n_number_0_capital_E", "[0E]"},
        {"n_number_0e+", "[0e+]"},
        {"n_number_0e", "[0e]"},
        {"n_number_1.0e+", "[1.0e+]"},
        {"n_number_1.0e-", "[1.0e-]"},
        {"n_number_1.0e", "[1.0e]"},
        {"n_number_1_000", "[1 000.0]"},
        {"n_number_1eE2", "[1eE2]"},
        {"n_number_2.e+3", "[2.e+3]"},
        {"n_number_2.e-3", "[2.e-3]"},
        {"n_number_2.e3", "[2.e3]"},
        {"n_number_9.e+", "[9.e+]"},
        {"n_number_Inf", "[Inf]"},
        {"n_number_NaN", "[NaN]"},
        {"n_number_U+FF11_fullwidth_digit_one", "[\xef\xbc\x91]"},
        {"n_number_expression", "[1+2]"},
        {"n_number_hex_1_digit", "[0x1]"},
        {"n_number_hex_2_digits", "[0x42]"},
        {"n_number_infinity", "[Infinity]"},
        {"n_number_invalid+-", "[0e+-1]"},
        {"n_number_invalid-negative-real", "[-123.123foo]"},
        {"n_number_minus_infinity", "[-Infinity]"},
        {"n_number_minus_sign_with_trailing_garbage", "[-foo]"},
        {"n_number_minus_space_1", "[- 1]"},
        {"n_number_neg_int_starting_with_zero", "[-012]"},
        {"n_number_neg_real_without_int_part", "[-.123]"},
        {"n_number_neg_with_garbage_at_end", "[-1x]"},
        {"n_number_real_garbage_after_e", "[1ea]"},
        {"n_number_real_without_fractional_part", "[1.]"},
        {"n_number_starting_with_dot", "[.123]"},
        {"n_number_with_alpha", "[1.2a-3]"},
        {"n_number_with_alpha_char", "[1.8011670033376514H-308]"},
        {"n_number_with_leading_zero", "[012]"},
        {"n_object_bad_value", "[\"x\", truth]"},
        {"n_object_bracket_key", "{[: \"x\"}"},
        {"n_object_comma_instead_of_colon", "{\"x\", null}"},
        {"n_object_double_colon", "{\"x\"::\"b\"}"},
        {"n_object_emoji", "{\xf0\x9f\x87\xa8\xf0\x9f\x87\xad}"},
        {"n_object_garbage_at_end", "{\"a\":\"a\" 123}"},
        {"n_object_key_with_single_quotes", "{key: 'value'}"},
        {"n_object_lone_continuation_byte_in_key_and_trailing_comma", "{\"\xb9\":\"0\",}"},
        {"n_object_missing_colon", "{\"a\" b}"},
        {"n_object_missing_key", "{:\"b\"}"},
        {"n_object_missing_semicolon", "{\"a\" \"b\"}"},
        {"n_object_missing_value", "{\"a\":"},
        {"n_object_no-colon", "{\"a\""},
        {"n_object_non_string_key", "{1:1}"},
        {"n_object_non_string_key_but_huge_number_instead", "{9999E9999:1}"},
        {"n_object_repeated_null_null", "{null:null,null:null}"},
        {"n_object_several_trailing_commas", "{\"id\":0,,,,,}"},
        {"n_object_single_quote", "{'a':0}"},
        {"n_object_trailing_comma", "{\"id\":0,}"},
        {"n_object_trailing_comment", "{\"a\":\"b\"}/**/"},
        {"n_object_trailing_comment_open", "{\"a\":\"b\"}/**//"},
        {"n_object_trailing_comment_slash_open", "{\"a\":\"b\"}//"},
        {"n_object_two_commas_in_a_row", "{\"a\":\"b\",,\"c\":\"d\"}"},
        {"n_object_unquoted_key", "{a: \"b\"}"},
        {"n_object_unterminated-value", "{\"a\":\"a"},
        {"n_object_with_single_string", "{ \"foo\" : \"bar\", \"a\" }"},
        {"n_object_with_trailing_garbage", "{\"a\":\"b\"}#"},
        {"n_single_space", " "},
        {"n_string_1_surrogate_then_escape", "[\"\\uD800\\\"]"},
        {"n_string_1_surrogate_then_escape_u", "[\"\\uD800\\u\"]"},
        {"n_string_1_surrogate_then_escape_u1", "[\"\\uD800\\u1\"]"},
        {"n_string_1_surrogate_then_escape_u1x", "[\"\\uD800\\u1x\"]"},
        {"n_string_accentuated_char_no_quotes", "[\xc3\xa9]"},
        {"n_string_backslash_00", "[\"\\\x00\"]"},
        {"n_string_escape_x", "[\"\\x00\"]"},
        {"n_string_escaped_backslash_bad", "[\"\\\\\\\"]"},
        {"n_string_escaped_ctrl_char_tab", "[\"\\\t\"]"},
        {"n_string_escaped_emoji", "[\"\\\xf0\x9f\x8c\x80\"]"},
        {"n_string_incomplete_escape", "[\"\\\"]"},
        {"n_string_incomplete_escaped_character", "[\"\\u00A\"]"},
        {"n_string_incomplete_surrogate", "[\"\\uD834\\uDd\"]"},
        {"n_string_incomplete_surrogate_escape_invalid", "[\"\\uD800\\uD800\\x\"]"},
        {"n_string_invalid-utf-8-in-escape", "[\"\\u\xe5\"]"},
        {"n_string_invalid_backslash_esc", "[\"\\a\"]"},
        {"n_string_invalid_unicode_escape", "[\"\\uqqqq\"]"},
        {"n_string_invalid_utf8_after_escape", "[\"\\\xe5\"]"},
        {"n_string_leading_uescaped_thinspace", "[\\u0020\"asd\"]"},
        {"n_string_no_quotes_with_bad_escape", "[\\n]"},
        {"n_string_single_doublequote", "\""},
        {"n_string_single_quote", "['single quote']"},
        {"n_string_single_string_no_double_quotes", "abc"},
        {"n_string_start_escape_unclosed", "[\"\\"},
        {"n_string_unescaped_ctrl_char", "[\"a\x00" "a\"]"},
        {"n_string_unescaped_newline", "[\"new\nline\"]"},
        {"n_string_unescaped_tab", "[\"\t\"]"},
        {"n_string_unicode_CapitalU", "\"\\UA66D\""},
        {"n_string_with_trailing_garbage", "\"\"x"},
        {"n_structure_U+2060_word_joined", "[\xe2\x81\xa0]"},
        {"n_structure_UTF8_BOM_no_data", "\xef\xbb\xbf"},
        {"n_structure_angle_bracket_.", "<.>"},
        {"n_structure_angle_bracket_null", "[<null>]"},
        {"n_structure_array_trailing_garbage", "[1]x"},
        {"n_structure_array_with_extra_array_close", "[1]]"},
        {"n_structure_array_with_unclosed_string", "[\"asd]"},
        {"n_structure_ascii-unicode-identifier", "a\xc3\xa5"},
        {"n_structure_capitalized_True", "[True]"},
        {"n_structure_close_unopened_array", "1]"},
        {"n_structure_comma_instead_of_closing_brace", "{\"x\": true,"},
        {"n_structure_double_array", "[][]"},
        {"n_structure_end_array", "]"},
        {"n_structure_incomplete_UTF8_BOM", "\xef\xbb{}"},
        {"n_structure_lone-invalid-utf-8", "\xe5"},
        {"n_structure_lone-open-bracket", "["},
        {"n_structure_no_data", ""},
        {"n_structure_null-byte-outside-string", "[\x00]"},
        {"n_structure_number_with_trailing_garbage", "2@"},
        {"n_structure_object_followed_by_closing_object", "{}}"},
        {"n_structure_object_unclosed_no_value", "{\"\":"},
        {"n_structure_object_with_comment", "{\"a\":/*comment*/\"b\"}"},
        {"n_structure_object_with_trailing_garbage", "{\"a\": true} \"x\""},
        {"n_structure_open_array_apostrophe", "['"},
        {"n_structure_open_array_comma", "[,"},
        {"n_structure_open_array_object", "[{\"\":[{\"\":[{\"\":"},
        {"n_structure_open_array_open_object", "[{"},
        {"n_structure_open_array_open_string", "[\"a"},
        {"n_structure_open_array_string", "[\"a\""},
        {"n_structure_open_object", "{"},
        {"n_structure_open_object_close_array", "{]"},
        {"n_structure_open_object_comma", "{,"},
        {"n_structure_open_object_open_array", "{["},
        {"n_structure_open_object_open_string", "{\"a"},
        {"n_structure_open_object_string_with_apostrophes", "{'a'"},
        {"n_structure_single_star", "*"},
        {"n_structure_trailing_#", "{\"a\":\"b\"}#{}"},
        {"n_structure_uescaped_LF_before_string", "[\\u000A\"\"]"},
        {"n_structure_unclosed_array", "[1"},
        {"n_structure_unclosed_array_partial_null", "[ false, nul"},
        {"n_structure_unclosed_array_unfinished_false", "[ true, fals"},
        {"n_structure_unclosed_array_unfinished_true", "[ false, tru"},
        {"n_structure_unclosed_object", "{\"asd\":\"asd\""},
        {"n_structure_unicode-identifier", "\xc3\xa5"},
        {"n_structure_whitespace_formfeed", "[\f]"},
    };
    for (iz i = 0; i < countof(no); i++) {
        expect(t, no[i].name, jsonverdict(&a, no[i].in), "reject");
    }

    // Implementation-defined: huge numbers parse (and convert to infinity
    // or zero), surrogate escapes and raw bytes pass through (writing
    // makes them U+FFFD), and non-UTF-8 encodings, BOMs, and deep nesting
    // are rejected.
    struct { Str name, in, want; } impl[] = {
        {"i_number_double_huge_neg_exp", "[123.456e-789]", "[123.456e-789]"},
        {"i_number_huge_exp", "[0.4e006699999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999969999999006]", {}},
        {"i_number_neg_int_huge_exp", "[-1e+9999]", "[-1e+9999]"},
        {"i_number_pos_double_huge_exp", "[1.5e+9999]", "[1.5e+9999]"},
        {"i_number_real_neg_overflow", "[-123123e100000]", "[-123123e100000]"},
        {"i_number_real_pos_overflow", "[123123e100000]", "[123123e100000]"},
        {"i_number_real_underflow", "[123e-10000000]", "[123e-10000000]"},
        {"i_number_too_big_neg_int", "[-123123123123123123123123123123]", "[-123123123123123123123123123123]"},
        {"i_number_too_big_pos_int", "[100000000000000000000]", "[100000000000000000000]"},
        {"i_number_very_big_negative_int", "[-237462374673276894279832749832423479823246327846]", {}},
        {"i_object_key_lone_2nd_surrogate", "{\"\\uDFAA\":0}", "{\"\xef\xbf\xbd\":0}"},
        {"i_string_1st_surrogate_but_2nd_missing", "[\"\\uDADA\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_1st_valid_surrogate_2nd_invalid", "[\"\\uD888\\u1234\"]", "[\"\xef\xbf\xbd\xe1\x88\xb4\"]"},
        {"i_string_UTF-16LE_with_BOM", "\xff\xfe[\x00\"\x00\xe9\x00\"\x00]\x00", "error at 0: unexpected character"},
        {"i_string_UTF-8_invalid_sequence", "[\"\xe6\x97\xa5\xd1\x88\xfa\"]", "[\"\xe6\x97\xa5\xd1\x88\xef\xbf\xbd\"]"},
        {"i_string_UTF8_surrogate_U+D800", "[\"\xed\xa0\x80\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_incomplete_surrogate_and_escape_valid", "[\"\\uD800\\n\"]", "[\"\xef\xbf\xbd\\n\"]"},
        {"i_string_incomplete_surrogate_pair", "[\"\\uDd1ea\"]", "[\"\xef\xbf\xbd" "a\"]"},
        {"i_string_incomplete_surrogates_escape_valid", "[\"\\uD800\\uD800\\n\"]", "[\"\xef\xbf\xbd\xef\xbf\xbd\\n\"]"},
        {"i_string_invalid_lonely_surrogate", "[\"\\ud800\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_invalid_surrogate", "[\"\\ud800abc\"]", "[\"\xef\xbf\xbd" "abc\"]"},
        {"i_string_invalid_utf-8", "[\"\xff\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_inverted_surrogates_U+1D11E", "[\"\\uDd1e\\uD834\"]", "[\"\xef\xbf\xbd\xef\xbf\xbd\"]"},
        {"i_string_iso_latin_1", "[\"\xe9\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_lone_second_surrogate", "[\"\\uDFAA\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_lone_utf8_continuation_byte", "[\"\x81\"]", "[\"\xef\xbf\xbd\"]"},
        {"i_string_not_in_unicode_range", "[\"\xf4\xbf\xbf\xbf\"]", "[\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\"]"},
        {"i_string_overlong_sequence_2_bytes", "[\"\xc0\xaf\"]", "[\"\xef\xbf\xbd\xef\xbf\xbd\"]"},
        {"i_string_truncated-utf-8", "[\"\xe0\xff\"]", "[\"\xef\xbf\xbd\xef\xbf\xbd\"]"},
        {"i_string_utf16BE_no_BOM", "\x00[\x00\"\x00\xe9\x00\"\x00]", "error at 0: unexpected character"},
        {"i_string_utf16LE_no_BOM", "[\x00\"\x00\xe9\x00\"\x00]\x00", "error at 1: unexpected character"},
        {"i_structure_UTF-8_BOM_empty_object", "\xef\xbb\xbf{}", "error at 0: unexpected character"},
    };
    for (iz i = 0; i < countof(impl); i++) {
        Str got  = jsoncanon(&a, impl[i].in);
        Str want = impl[i].want.data ? impl[i].want : impl[i].in;
        expect(t, impl[i].name, got, want);
    }
    expect(t, "i_structure_500_nested_arrays",
           jsonverdict(&a, nested(&a, 500, "[", "]")), "reject");

    // Canonical text comes back byte for byte.
    Str canon[] = {
        "{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"e\"}}",
        "[[],{},[[]],{\"\":{}}]",
        "{\"z\":1,\"a\":2,\"m\":3}",
        "{\"a\":\"b\",\"a\":\"c\"}",
        "[0,-0,1.5,-2.5e-3,1E400,123456789012345678901234567890]",
        "\"\\u0000\\u001f\\b\\f\\n\\r\\t\\\"\\\\/\x7f\"",
        "\"\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x98\x80\xe2\x80\xa8\"",
        "{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"}}]}",
        "\"\"",
        "null",
    };
    for (iz i = 0; i < countof(canon); i++) {
        expect(t, canon[i], jsoncanon(&a, canon[i]), canon[i]);
    }

    // Non-canonical text becomes canonical.
    struct { Str in, want; } recanon[] = {
        {" { \"a\" : 1 ,\n\t\"b\" : [ 1 , 2 ] }\r\n", "{\"a\":1,\"b\":[1,2]}"},
        {"\"\\u0041\\/\"", "\"A/\""},
        {"\"\\u00e9\\u20AC\"", "\"\xc3\xa9\xe2\x82\xac\""},
        {"\"\\uD83D\\uDE00\"", "\"\xf0\x9f\x98\x80\""},
        {"\"\\uD800\\uD800\\uDC00\"", "\"\xef\xbf\xbd\xf0\x90\x80\x80\""},
        {"\"\\uDC00\\uD800\"", "\"\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\"\\u001F\\u007f\"", "\"\\u001f\x7f\""},
        {"\"\\u0000x\"", "\"\\u0000x\""},
        // Raw bytes: WTF-8 halves join, everything invalid is U+FFFD
        {"\"\xed\xa0\xbd\xed\xb8\x80\"", "\"\xf0\x9f\x98\x80\""},
        {"\"\xed\xb8\x80\xed\xa0\xbd\"", "\"\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\"\xe2\x82\"", "\"\xef\xbf\xbd\""},
        {"\"\xe2\x82x\"", "\"\xef\xbf\xbdx\""},
        {"\"\xe0\x80\x80\"", "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\"\xf4\x90\x80\x80\"", "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\"\xf0\x9f\x98\"", "\"\xef\xbf\xbd\""},
        {"\"\xc2\"", "\"\xef\xbf\xbd\""},
        {"\"\xf5\xf8\xfe\xff\"", "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\""},
    };
    for (iz i = 0; i < countof(recanon); i++) {
        expect(t, recanon[i].in, jsoncanon(&a, recanon[i].in), recanon[i].want);
    }

    // Decoded strings, before any writing: surrogate escapes are joined
    // or replaced, while raw bytes pass through untouched.
    struct { Str in, want; } decoded[] = {
        {"\"\\uD800\"", "\xef\xbf\xbd"},
        {"\"\\uD83D\\uDE00\"", "\xf0\x9f\x98\x80"},
        {"\"\\uDE00\\uD83D\"", "\xef\xbf\xbd\xef\xbf\xbd"},
        {"\"\\uD800\\uD800\\uDC00\"", "\xef\xbf\xbd\xf0\x90\x80\x80"},
        {"\"\\uD800\\n\"", "\xef\xbf\xbd\n"},
        {"\"a\\u0000b\"", Str("a\0b")},
        {"\"\\u00e9\\u0800\\uFFFF\"", "\xc3\xa9\xe0\xa0\x80\xef\xbf\xbf"},
        {"\"\\n\\t\\\"\\\\\\/\\b\\f\\r\"", "\n\t\"\\/\b\f\r"},
        {"\"\xff\xed\xa0\x80\xc0\xaf\"", "\xff\xed\xa0\x80\xc0\xaf"},
        {"\"plain\"", "plain"},
    };
    for (iz i = 0; i < countof(decoded); i++) {
        Str got = jsonstr(jsonparse(&a, decoded[i].in).root);
        expect(t, decoded[i].in, got, decoded[i].want);
    }

    // Error positions and messages
    struct { Str in, want; } errs[] = {
        {"", "error at 0: unexpected end of input"},
        {"  ", "error at 2: unexpected end of input"},
        {"[1,]", "error at 3: unexpected character"},
        {"[1 2]", "error at 3: expected ',' or ']'"},
        {"{\"a\" 1}", "error at 5: expected ':'"},
        {"{\"a\":1,}", "error at 7: expected string key"},
        {"{\"a\":1 \"b\"}", "error at 7: expected ',' or '}'"},
        {"\"abc", "error at 4: unterminated string"},
        {"\"abc\\", "error at 5: unterminated string"},
        {"\"a\\x\"", "error at 2: invalid escape"},
        {"\"a\\u12g4\"", "error at 2: invalid \\u escape"},
        {"\"a\nb\"", "error at 2: control character in string"},
        {"[01]", "error at 2: leading zero in number"},
        {"-", "error at 1: invalid number"},
        {"1.e5", "error at 2: invalid number"},
        {"1e+", "error at 3: invalid number"},
        {"tru", "error at 0: invalid literal"},
        {"nulL", "error at 0: invalid literal"},
        {"[1] x", "error at 4: unexpected data after the value"},
        {"truex", "error at 4: unexpected data after the value"},
    };
    for (iz i = 0; i < countof(errs); i++) {
        expect(t, errs[i].in, jsoncanon(&a, errs[i].in), errs[i].want);
    }

    // Nesting: 64 levels parse, 65 do not
    expect(t, "depth 64 arrays", jsonverdict(&a, nested(&a, 64, "[", "]")), "accept");
    expect(t, "depth 64 arrays canon", jsoncanon(&a, nested(&a, 64, "[", "]")),
           nested(&a, 64, "[", "]"));
    expect(t, "depth 65 arrays", jsoncanon(&a, nested(&a, 65, "[", "]")),
           "error at 64: nested too deeply");
    expect(t, "depth 64 objects", jsonverdict(&a, nestedobj(&a, 64)), "accept");
    expect(t, "depth 65 objects", jsoncanon(&a, nestedobj(&a, 65)),
           "error at 320: nested too deeply");

    // Writing nests no deeper than reading: a document the reader took,
    // written inside another container, keeps what fits, and the arrays
    // or objects past the limit become null, so the result parses again.
    {
        Str deep63  = nested(&a, 63, "[", "]");
        Str deep64  = nested(&a, 64, "[", "]");
        Str obj63   = nestedobj(&a, 63);
        Str obj64   = nestedobj(&a, 64);
        Str cut64   = concat(&a, takehead(deep64, 63), "null");
        cut64 = concat(&a, cut64, cuthead(deep64, 65));
        Str cutobj  = concat(&a, takehead(obj64, 63*5), "null");
        cutobj = concat(&a, cutobj, cuthead(obj64, 63*5+2));
        struct { Str name, in, want; } embeds[] = {
            {"embed 63 arrays",  deep63, deep63},
            {"embed 64 arrays",  deep64, cut64},
            {"embed 63 objects", obj63,  obj63},
            {"embed 64 objects", obj64,  cutobj},
            {"embed scalar",     "[[1]]", "[[1]]"},
        };
        for (iz i = 0; i < countof(embeds); i++) {
            Str got  = jsonembed(&a, embeds[i].in);
            Str want = concat(&a, "{\"v\":", embeds[i].want);
            want = concat(&a, want, "}");
            expect(t, embeds[i].name, got, want);
            expect(t, embeds[i].name, jsonverdict(&a, got), "accept");
        }
    }

    // Written by hand, the full depth closes with the right brackets.
    {
        Buf     b(&a, 512);
        JsonOut w = {&b};
        for (i32 i = 0; i < JSON_MAXDEPTH; i++) {
            if (i % 2) {
                putarray(&w);
            } else {
                putobject(&w);
                putkey(&w, "k");
            }
        }
        for (i32 i = 0; i < JSON_MAXDEPTH; i++) {
            putend(&w);
        }
        Str got = finish(&b);
        expect(t, "writer full depth", got, nested(&a, 32, "{\"k\":[", "]}"));
        expect(t, "writer full depth", jsonverdict(&a, got), "accept");
        expect(t, "writer full depth closed", istr(&a, w.depth), "0");
    }

    // A failed parse leaves nothing allocated.
    {
        Arena   b    = a;
        byte   *save = b.beg;
        JsonDoc d    = jsonparse(&b, "[[1,2,{\"x\":\"\\n\"}],3,");
        expect(t, "failed parse frees", istr(&b, b.beg==save && !d.ok), "1");
        expect(t, "failed parse root", jsonshow(&b, d.root), "missing");
    }

    // Accessors, including chains through missing values
    Str src =
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"hi\\n\",\"tool_calls\":[{\"id\":\"c1\",\"function\":"
        "{\"name\":\"read\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}],"
        "\"usage\":{\"prompt_tokens\":12,\"total\":3.0,\"big\":1e2,"
        "\"frac\":1.5,\"max\":9223372036854775807,"
        "\"over\":9223372036854775808,\"neg\":-0,\"ok\":true},"
        "\"dup\":1,\"dup\":2,\"a\\u0062\":\"esc\",\"foo\\u0000bar\":7,"
        "\"args\": {\"x\": [1, 2]} }";
    JsonDoc d = jsonparse(&a, src);
    Json   *r = d.root;
    Json   *m = jsonget(jsonat(jsonget(r, "choices"), 0), "message");
    expect(t, "parse ok", istr(&a, d.ok), "1");
    expect(t, "chain", jsonshow(&a, jsonget(m, "content")), "string hi\n");
    expect(t, "chain deep", jsonstr(jsonget(jsonget(jsonat(
           jsonget(m, "tool_calls"), 0), "function"), "name")), "read");
    expect(t, "missing key", jsonshow(&a, jsonget(m, "nope")), "missing");
    expect(t, "missing chain", jsonshow(&a, jsonget(jsonat(jsonget(jsonget(
           r, "nope"), "x"), 3), "y")), "missing");
    expect(t, "index range", jsonshow(&a, jsonat(jsonget(r, "choices"), 1)), "missing");
    expect(t, "index negative", jsonshow(&a, jsonat(jsonget(r, "choices"), -1)), "missing");
    expect(t, "index of scalar", jsonshow(&a, jsonat(jsonget(m, "role"), 0)), "missing");
    expect(t, "key of array", jsonshow(&a, jsonget(jsonget(r, "choices"), "")), "missing");
    expect(t, "member by index", jsonshow(&a, jsonat(m, 1)), "string hi\n");
    expect(t, "null value", jsonshow(&a, jsonget(jsonat(jsonget(r, "choices"), 0),
           "finish_reason")), "null");
    expect(t, "len object", istr(&a, jsonlen(r)), "7");
    expect(t, "len array", istr(&a, jsonlen(jsonget(r, "choices"))), "1");
    expect(t, "len scalar", istr(&a, jsonlen(jsonget(m, "role"))), "0");
    expect(t, "len missing", istr(&a, jsonlen(jsonget(r, "nope"))), "0");
    expect(t, "dup last wins", istr(&a, jsonint(jsonget(r, "dup"), 0)), "2");
    expect(t, "escaped key", jsonstr(jsonget(r, "ab")), "esc");
    expect(t, "nul in key", istr(&a, jsonint(jsonget(r, Str("foo\0bar")), 0)), "7");
    expect(t, "str of number", istr(&a, !jsonstr(jsonget(r, "dup")).data), "1");
    expect(t, "str of missing", istr(&a, !jsonstr(jsonget(r, "nope")).data), "1");
    expect(t, "empty str present", istr(&a, !!jsonstr(jsonparse(&a, "\"\"").root).data), "1");
    expect(t, "raw args", jsonget(r, "args")->raw, "{\"x\": [1, 2]}");
    expect(t, "raw string", jsonget(r, "ab")->raw, "\"esc\"");

    Json *u = jsonget(r, "usage");
    expect(t, "int", istr(&a, jsonint(jsonget(u, "prompt_tokens"), -1)), "12");
    expect(t, "int from 3.0", istr(&a, jsonint(jsonget(u, "total"), -1)), "3");
    expect(t, "int from 1e2", istr(&a, jsonint(jsonget(u, "big"), -1)), "100");
    expect(t, "int from 1.5", istr(&a, jsonint(jsonget(u, "frac"), -1)), "-1");
    expect(t, "int max", istr(&a, jsonint(jsonget(u, "max"), -1)), "9223372036854775807");
    expect(t, "int over", istr(&a, jsonint(jsonget(u, "over"), -1)), "-1");
    expect(t, "int neg zero", istr(&a, jsonint(jsonget(u, "neg"), -1)), "0");
    expect(t, "int of bool", istr(&a, jsonint(jsonget(u, "ok"), -1)), "-1");
    expect(t, "int missing", istr(&a, jsonint(jsonget(u, "nope"), 42)), "42");
    expect(t, "float", fnum(&a, jsonfloat(jsonget(u, "frac"), -1)), "1.5");
    expect(t, "float of int", fnum(&a, jsonfloat(jsonget(u, "prompt_tokens"), -1)), "12");
    expect(t, "float of string", fnum(&a, jsonfloat(jsonget(m, "role"), -1)), "-1");
    expect(t, "bool", istr(&a, jsonbool(jsonget(u, "ok"), 0)), "1");
    expect(t, "bool of number", istr(&a, jsonbool(jsonget(u, "total"), 0)), "0");
    expect(t, "bool missing", istr(&a, jsonbool(jsonget(u, "nope"), 1)), "1");

    // Integers are judged exactly on the text, however a double would
    // round it.
    struct { Str in, want; } ints[] = {
        {"0", "0"},
        {"-0", "0"},
        {"-12", "-12"},
        {"1E+2", "100"},
        {"100e-2", "1"},
        {"120e-1", "12"},
        {"-1.20e1", "-12"},
        {"0.1e1", "1"},
        {"0.000e-5", "0"},
        {"-0.0e-0", "0"},
        {"0e999999999999999999999", "0"},
        {"1e18", "1000000000000000000"},
        {"9.223372036854775807e18", "9223372036854775807"},
        {"922337203685477580.7e1", "9223372036854775807"},
        {"-922337203685477580.80e1", "-9223372036854775808"},
        {"-9223372036854775808", "-9223372036854775808"},
        {"9007199254740993.0", "9007199254740993"},
        {"123456789012345678e1", "1234567890123456780"},
        {"10000000000000000000e-1", "1000000000000000000"},
        {"0.0000000000000000000000000000001e31", "1"},
        {"1.5", "default"},
        {"125e-1", "default"},
        {"0.01e1", "default"},
        {"4.0000000000000001", "default"},
        {"0.99999999999999999", "default"},
        {"1e-400", "default"},
        {"1e19", "default"},
        {"9223372036854775808", "default"},
        {"-9223372036854775809", "default"},
        {"9.223372036854775808e18", "default"},
        {"1e99999999999999999999", "default"},
        {"-1e99999999999999999999", "default"},
        {"\"1\"", "default"},
    };
    for (iz i = 0; i < countof(ints); i++) {
        expect(t, ints[i].in, jsonintstr(&a, ints[i].in), ints[i].want);
    }
    {
        // Random integers with the point moved by an exponent come back
        // exactly, and a trailing nonzero digit makes them fractions.
        u64 rng = 7;
        i32 bad = 0;
        for (i32 i = 0; i < 10000; i++) {
            Arena tmp = a;
            rng = rng*0x3243f6a8885a308d + 1;
            i64 v      = (i64)rng >> (rng>>58);
            Str s      = istr(&tmp, v);
            b32 neg    = s[0] == '-';
            Str digits = cuthead(s, neg);
            iz  point  = 1 + (iz)(rng>>20 & 0xffff)%digits.len;
            Buf b(&tmp, 64);
            if (neg) putbyte(&b, '-');
            print(&b, takehead(digits, point));
            putbyte(&b, '.');
            print(&b, cuthead(digits, point));
            print(&b, "00");
            Str head = finish(&b);
            Str exp  = istr(&tmp, digits.len - point);
            Str in   = concat(&tmp, concat(&tmp, head, "e"), exp);
            Str frac = concat(&tmp, concat(&tmp, head, "1e"), exp);
            bad += !(jsonintstr(&tmp, in) == s);
            bad += !(jsonintstr(&tmp, frac) == "default");
        }
        expect(t, "random ints", istr(&a, bad), "0");
    }
    {
        Buf b(&a, 64);
        for (Json *e = r->child; e; e = e->next) {
            print(&b, e->key.len ? e->key : Str("?"));
            putbyte(&b, ' ');
        }
        expect(t, "iterate members", finish(&b),
               Str("choices usage dup dup ab foo\0bar args "));
        i32 n = 0;
        for (Json *e = jsonget(r, "nope")->child; e; e = e->next) n++;
        expect(t, "iterate missing", istr(&a, n), "0");
    }
    {
        Buf     b(&a, 64);
        JsonOut w = {&b};
        putjson(&w, jsonget(r, "nope"));
        expect(t, "write missing", finish(&b), "null");
    }

    // Writer: comma and colon bookkeeping, key order, every value type
    {
        Buf     b(&a, 64);
        JsonOut w = {&b};
        putobject(&w);
        putkey(&w, "z");
        putint(&w, -9223372036854775807 - 1);
        putkey(&w, "a");
        putarray(&w);
        putbool(&w, 1);
        putbool(&w, 0);
        putnull(&w);
        putarray(&w);
        putend(&w);
        putobject(&w);
        putend(&w);
        putstr(&w, "");
        putend(&w);
        putkey(&w, "raw");
        putraw(&w, "{\"x\": 1}");
        putkey(&w, "f");
        putfloat(&w, 0.25);
        putkey(&w, "o");
        putobject(&w);
        putkey(&w, "k");
        putstr(&w, "v");
        putend(&w);
        putend(&w);
        expect(t, "writer", finish(&b),
               "{\"z\":-9223372036854775808,\"a\":[true,false,null,[],{},\"\"],"
               "\"raw\":{\"x\": 1},\"f\":0.25,\"o\":{\"k\":\"v\"}}");
        expect(t, "writer depth", istr(&a, w.depth), "0");
    }
    {
        Buf     b(&a, 64);
        JsonOut w = {&b};
        putint(&w, 1);
        putstr(&w, "x");
        expect(t, "writer top level", finish(&b), "1\"x\"");
    }

    // String escaping always yields valid UTF-8 JSON
    {
        Buf  b(&a, 64);
        u8   all[32];
        for (i32 i = 0; i < 32; i++) all[i] = (u8)i;
        printjsonstr(&b, {all, 32});
        expect(t, "escape controls", finish(&b),
               "\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
               "\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012"
               "\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a"
               "\\u001b\\u001c\\u001d\\u001e\\u001f\"");
    }
    struct { Str in, want; } esc[] = {
        {"plain", "\"plain\""},
        {"", "\"\""},
        {"q\"b\\s/", "\"q\\\"b\\\\s/\""},
        {"del\x7f", "\"del\x7f\""},
        {"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", "\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\""},
        {"\xef\xbf\xbf\xf4\x8f\xbf\xbf", "\"\xef\xbf\xbf\xf4\x8f\xbf\xbf\""},
        {"a\x80z", "\"a\xef\xbf\xbdz\""},
        {"\xc1\xbf", "\"\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\xed\x9f\xbf", "\"\xed\x9f\xbf\""},
        {"\xed\xa0\x80", "\"\xef\xbf\xbd\""},
        {"\xed\xbf\xbf", "\"\xef\xbf\xbd\""},
        {"\xed\xa0\x80\xed\xb0\x80", "\"\xf0\x90\x80\x80\""},
        {"\xed\xaf\xbf\xed\xbf\xbf", "\"\xf4\x8f\xbf\xbf\""},
        {"\xed\xa0\x80\xed\xa0\x80\xed\xb0\x80", "\"\xef\xbf\xbd\xf0\x90\x80\x80\""},
        {"\xed\xa0\x80" "a", "\"\xef\xbf\xbd" "a\""},
        {"\xed\xa0", "\"\xef\xbf\xbd\""},
        {"x\xf0\x90\x80", "\"x\xef\xbf\xbd\""},
        {"\xf0\x80\x80\x80", "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\""},
        {"\xe2\x80\xa8\xe2\x80\xa9", "\"\xe2\x80\xa8\xe2\x80\xa9\""},
    };
    for (iz i = 0; i < countof(esc); i++) {
        Buf b(&a, 16);
        printjsonstr(&b, esc[i].in);
        expect(t, esc[i].want, finish(&b), esc[i].want);
    }

    // Numbers: up to 15 significant digits, shortest form
    struct { f64 v; Str want; } nums[] = {
        {0, "0"},
        {-0.0, "0"},
        {1, "1"},
        {-3, "-3"},
        {0.1, "0.1"},
        {0.7, "0.7"},
        {0.95, "0.95"},
        {0.1+0.2, "0.3"},
        {1.0/3, "0.333333333333333"},
        {2.0/3, "0.666666666666667"},
        {42.5, "42.5"},
        {1234.5678, "1234.5678"},
        {100, "100"},
        {1e15, "1000000000000000"},
        {123456789012345678.0, "123456789012346000"},
        {1e20, "100000000000000000000"},
        {1e21, "1e21"},
        {1.5e300, "1.5e300"},
        {1.7976931348623157e308, "1.79769313486232e308"},
        {0.000001, "0.000001"},
        {0.0000015, "0.0000015"},
        {1e-7, "1e-7"},
        {-2.5e-9, "-2.5e-9"},
        {2.2250738585072014e-308, "2.2250738585072e-308"},
        {5e-324, "4.94065645841247e-324"},
        {9.999999999999999e14, "1000000000000000"},
        {__builtin_inf(), "null"},
        {-__builtin_inf(), "null"},
        {__builtin_nan(""), "null"},
    };
    for (iz i = 0; i < countof(nums); i++) {
        expect(t, nums[i].want, fnum(&a, nums[i].v), nums[i].want);
    }

    // Short decimals survive printing and parsing exactly.
    {
        Str decimals[] = {
            "0.1", "0.2", "0.3", "1.1", "2.675", "1e-5", "123.456", "99.99",
            "0.07", "3.14159265358979", "6.02214076e23", "1.6e-19",
        };
        for (iz i = 0; i < countof(decimals); i++) {
            f64 v = parsefloat(decimals[i]).val;
            f64 w = parsefloat(fnum(&a, v)).val;
            expect(t, decimals[i], istr(&a, v == w), "1");
        }

        // Random 15-digit decimals from 1e-8 to 1e37, where parsefloat
        // rounds once and the printer promises the same digits back.
        // (Parsing the output again may round twice, so compare digits.)
        expect(t, "decnorm", decnorm(&a, "1.50e3"), "15e3");
        expect(t, "decnorm int", decnorm(&a, "1500"), "15e3");
        expect(t, "decnorm frac", decnorm(&a, "0.0015e6"), "15e3");
        expect(t, "decnorm neg", decnorm(&a, "0.000001"), "1e-6");
        u64 rng = 1;
        i32 bad = 0;
        for (i32 i = 0; i < 100000; i++) {
            Arena tmp = a;
            Buf   b(&tmp, 64);
            rng = rng*0x3243f6a8885a308d + 1;
            print(&b, (i64)(rng>>20)%900000000000000 + 100000000000000);
            putbyte(&b, 'e');
            print(&b, (i64)(rng>>8 & 63)%45 - 22);
            Str in  = finish(&b);
            Str out = fnum(&tmp, parsefloat(in).val);
            bad += !(decnorm(&tmp, in) == decnorm(&tmp, out));
        }
        expect(t, "random digits", istr(&a, bad), "0");
    }
}
