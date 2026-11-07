#include "markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void want(const char *html, const char *needle, const char *what) {
    if (!strstr(html, needle)) {
        fprintf(stderr, "FAIL %s: missing %s\n", what, needle);
        failures++;
    }
}

static void reject(const char *html, const char *needle, const char *what) {
    if (strstr(html, needle)) {
        fprintf(stderr, "FAIL %s: found %s\n", what, needle);
        failures++;
    }
}

int main(void) {
    const char *src =
        "# 标题 <script>alert(1)</script>\n\n"
        "**粗体** *斜体* ~~删除~~ `a<b`\n\n"
        "[好链接](https://example.com) [坏链接](javascript:alert(1))\n\n"
        "![好图](https://example.com/a.png) ![坏图](data:image/svg+xml,x)\n\n"
        "> 引用\n> 第二行\n\n"
        "- 一\n- 二\n\n"
        "1. 甲\n2. 乙\n\n"
        "```c\nint x = 1 < 2;\n```\n\n"
        "| 名称 | 值 |\n| :--- | ---: |\n| a | 1 |\n\n"
        "---\n";
    size_t len = 0;
    char *html = md_render_html(src, strlen(src), &len);
    if (!html) { fprintf(stderr, "FAIL render returned NULL\n"); return 1; }
    if (len != strlen(html)) { fprintf(stderr, "FAIL length\n"); failures++; }

    want(html, "<h1>标题 ", "heading");
    want(html, "<strong>粗体</strong>", "strong");
    want(html, "<em>斜体</em>", "em");
    want(html, "<del>删除</del>", "del");
    want(html, "<code>a&lt;b</code>", "code");
    want(html, "<blockquote>", "quote");
    want(html, "<ul>", "ul");
    want(html, "<ol>", "ol");
    want(html, "<pre><code class=\"language-c\">", "fence");
    want(html, "<table>", "table");
    want(html, "<th>名称</th>", "th");
    want(html, "<hr>", "hr");
    want(html, "href=\"https://example.com\"", "safe link");
    want(html, "<img src=\"https://example.com/a.png\"", "safe image");

    reject(html, "<script", "raw script");
    reject(html, "javascript:", "javascript url");
    reject(html, "data:image", "data image");
    reject(html, "onerror=", "event attribute");

    free(html);
    if (failures) return 1;
    puts("markdown: OK");
    return 0;
}
