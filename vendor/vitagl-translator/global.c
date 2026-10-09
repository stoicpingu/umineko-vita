/*
 * This file is part of vitaGL
 * Copyright 2017-2023 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* Native targeted replacement; see UPSTREAM.md. */
#define _GNU_SOURCE
#include "bridge.h"
#include <stdlib.h>

/* Replacements may be shorter than their GLSL declarations. The parser below
 * temporarily NUL-terminates the variable name; overwrite that byte as padding
 * too, or a known fragment varying silently truncates the entire shader. */
static void write_declaration(char *begin, char *end, const char *replacement) {
	size_t available = (size_t)(end - begin);
	size_t length = strlen(replacement);
	if (length > available) {
		vgl_log("vitaGL: translated varying exceeds its declaration width\n");
		abort();
	}
	memcpy(begin, replacement, length);
	memset(begin + length, ' ', available - length);
}

void glsl_translate_with_global(char *text, GLenum type, GLboolean hasFrontFacing) {
	char newline[128];
	int idx;
	if (type == GL_VERTEX_SHADER) {
		// Manually patching attributes and varyings
		char *str = strstr(text, "attribute");
		while (str && !(str[9] == ' ' || str[9] == '\t')) {
			str = strstr(str + 9, "attribute");
		}
		char *str2 = strstr(text, "varying");
		while (str2 && !(str2[7] == ' ' || str2[7] == '\t')) {
			str2 = strstr(str2 + 7, "varying");
		}
		while (str || str2) {
			char *t;
			if (!str)
				t = str2;
			else if (!str2)
				t = str;
			else
				t = min(str, str2);
			if (t == str) { // Attribute
				// Replace attribute with 'vgl in' that will get extended in a 'varying in' by the preprocessor
				vgl_fast_memcpy(t, "vgl in    ", 10);
				str = strstr(t, "attribute");
				while (str && !(str[9] == ' ' || str[9] == '\t')) {
					str = strstr(str + 9, "attribute");
				}
			} else { // Varying
				// t is the selected varying; str may be NULL after the last attribute.
				char *end = strstr(t, ";");
				char *declaration_end = end + 1;
				GLboolean name_started = GL_FALSE;
				char *start = end;
				while ((*start != ' ' && *start != '\t') || !name_started) {
					if (!name_started && *start != ' ' && *start != '\t' && *start != ';')
						name_started = GL_TRUE;
					if (!name_started) {
						end--;
					}
					start--;
				}
				end++;
				start++;
				end[0] = 0;
				idx = -1;
				// Check first if the varying has a known binding
				for (int j = 0; j < glsl_custom_bindings_num; j++) {
					if (!strcmp(glsl_custom_bindings[j].name, start)) {
						glsl_custom_bindings[j].ref_idx = glsl_current_ref_idx;
						idx = j;
					}
				}
				if (idx != -1) {
					switch (glsl_custom_bindings[idx].type) {
					case VGL_TYPE_TEXCOORD:
						{
							if (glsl_custom_bindings[idx].idx != -1) {
								strcpy(glsl_texcoords_binds[glsl_custom_bindings[idx].idx], start);
								glsl_texcoords_used[glsl_custom_bindings[idx].idx] = GL_TRUE;
								sprintf(newline, "VOUT(%s,%d);", str2 + 8, glsl_custom_bindings[idx].idx);
							} else {
								sprintf(newline, "VOUT(%s,\v);", str2 + 8);
							}
						}
						break;
					case VGL_TYPE_COLOR:
						{
							if (glsl_custom_bindings[idx].idx != -1) {
								strcpy(glsl_colors_binds[glsl_custom_bindings[idx].idx], start);
								glsl_colors_used[glsl_custom_bindings[idx].idx] = GL_TRUE;
								sprintf(newline, "COUT(%s,%d);", str2 + 8, glsl_custom_bindings[idx].idx);
							} else {
								sprintf(newline, "COUT(%s,\f);", str2 + 8);
							}
						}
						break;
					case VGL_TYPE_FOG:
						sprintf(newline, "FOUT(%s,%d);", str2 + 8, 0);
						break;
					case VGL_TYPE_CLIP:
						sprintf(newline, "POUT(%s,%d);", str2 + 8, glsl_custom_bindings[idx].idx);
						break;
					}
				} else {
					sprintf(newline, "VOUT(%s,\v);", str2 + 8);
				}
				write_declaration(str2, declaration_end, newline);
				str2 = strstr(t, "varying");
				while (str2 && !(str2[7] == ' ' || str2[7] == '\t')) {
					str2 = strstr(str2 + 7, "varying");
				}
			}
		}
	} else {
		// Manually patching gl_FrontFacing usage
		if (hasFrontFacing) {
			char *str = strstr(text, "gl_FrontFacing");
			while (str) {
				vgl_fast_memcpy(str, "(vgl_Face > 0)", 14);
				str = strstr(str, "gl_FrontFacing");
			}
		}
		// Manually patching varyings and "texture" uniforms
		char *str = strstr(text, "varying");
		while (str && !(str[7] == ' ' || str[7] == '\t')) {
			str = strstr(str + 1, "varying");
		}
		char *str2 = strcasestr(text, "texture");
		while (str2) {
			char *str2_end = str2 + 7;
			if (*(str2 - 1) == ' ' || *(str2 - 1) == '\t' || *(str2 - 1) == '(') {
				while (*str2_end == ' ' || *str2_end == '\t') {
					str2_end++;
				}
				if (*str2_end == ',' || *str2_end == ';')
					break;
			}
			str2 = strcasestr(str2_end, "texture");
		}
		while (str || str2) {
			char *t;
			if (!str)
				t = str2;
			else if (!str2)
				t = str;
			else
				t = min(str, str2);
			if (t == str) { // Varying
				char *end = strstr(str, ";");
				char *declaration_end = end + 1;
				GLboolean name_started = GL_FALSE;
				char *start = end;
				while ((*start != ' ' && *start != '\t') || !name_started) {
					if (!name_started && *start != ' ' && *start != '\t' && *start != ';')
						name_started = GL_TRUE;
					if (!name_started) {
						end--;
					}
					start--;
				}
				end++;
				start++;
				end[0] = 0;
				idx = -1;
				// Check first if the varying has a known binding
				for (int j = 0; j < glsl_custom_bindings_num; j++) {
					if (!strcmp(glsl_custom_bindings[j].name, start)) {
						glsl_custom_bindings[j].ref_idx = glsl_current_ref_idx;
						idx = j;
					}
				}
				if (idx != -1) {
					switch (glsl_custom_bindings[idx].type) {
					case VGL_TYPE_TEXCOORD:
						{
							if (glsl_custom_bindings[idx].idx != -1) {
								strcpy(glsl_texcoords_binds[glsl_custom_bindings[idx].idx], start);
								glsl_texcoords_used[glsl_custom_bindings[idx].idx] = GL_TRUE;
								sprintf(newline, "VIN(%s,%d);", str + 8, glsl_custom_bindings[idx].idx);
							} else {
								sprintf(newline, "VIN(%s,\v);", str + 8);
							}
						}
						break;
					case VGL_TYPE_COLOR:
						{
							if (glsl_custom_bindings[idx].idx != -1) {
								strcpy(glsl_colors_binds[glsl_custom_bindings[idx].idx], start);
								glsl_colors_used[glsl_custom_bindings[idx].idx] = GL_TRUE;
								sprintf(newline, "CIN(%s,%d);", str + 8, glsl_custom_bindings[idx].idx);
							} else {
								sprintf(newline, "CIN(%s,\f);", str + 8);
							}
						}
						break;
					case VGL_TYPE_FOG:
						sprintf(newline, "FIN(%s, %d);", str + 8, glsl_custom_bindings[idx].idx);
						break;
					case VGL_TYPE_CLIP:
						vgl_log("%s:%d %s: Unexpected varying type (VGL_TYPE_CLIP) for %s in fragment shader.\n", __FILE__, __LINE__, __func__, str + 8);
						break;
					}
				} else {
					sprintf(newline, "VIN(%s, \v);", str + 8);
				}
				write_declaration(str, declaration_end, newline);
				str = strstr(str, "varying");
				while (str && !(str[7] == ' ' || str[7] == '\t')) {
					str = strstr(str + 7, "varying");
				}
			} else { // "texture" Uniform
				if (t[0] == 't')
					vgl_fast_memcpy(t, "vgl_tex", 7);
				else
					vgl_fast_memcpy(t, "Vgl_tex", 7);
				str2 = strcasestr(t, "texture");
				while (str2) {
					char *str2_end = str2 + 7;
					if (*(str2 - 1) == ' ' || *(str2 - 1) == '\t' || *(str2 - 1) == '(') {
						while (*str2_end == ' ' || *str2_end == '\t') {
							str2_end++;
						}
						if (*str2_end == ',' || *str2_end == ';')
							break;
					}
					str2 = strcasestr(str2_end, "texture");
				}
			}
		}
	}
}

