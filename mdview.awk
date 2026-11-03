# mdview.awk -- render Markdown for a terminal with ANSI escapes.
#
# Part of etmux: used by open-file (file-markdown-command) when no other
# Markdown viewer (glow, mdcat) is installed. Plain POSIX awk so it runs
# with mawk, gawk or busybox awk:
#
#	awk -f mdview.awk README.md | less -R
#
# Handles headings, emphasis, inline code, fenced and indented code blocks,
# block quotes, lists, task lists, horizontal rules, links and images.
# Tables and HTML are shown as they are.

BEGIN {
	E = "\033["
	RESET = E "0m"
	BOLD = E "1m"; NOBOLD = E "22m"
	DIM = E "2m"
	ITALIC = E "3m"; NOITALIC = E "23m"
	UNDER = E "4m"; NOUNDER = E "24m"
	ACCENT = E "38;5;111m"		# blue
	ACCENT2 = E "38;5;183m"		# lavender
	CODE = E "38;5;150m"		# green
	MUTED = E "38;5;244m"
	FG = E "39m"
	WIDTH = 72
	incode = 0
}

function rule(ch, n,	s, i) {
	s = ""
	for (i = 0; i < n; i++)
		s = s ch
	return (s)
}

# Replace each delimited span (open..close) with on text off. The
# delimiter is matched literally.
function span(s, delim, on, off,	out, i, j, rest, n) {
	out = ""
	n = length(delim)
	while ((i = index(s, delim)) > 0) {
		rest = substr(s, i + n)
		j = index(rest, delim)
		if (j <= 1)
			break
		out = out substr(s, 1, i - 1) on substr(rest, 1, j - 1) off
		s = substr(rest, j + n)
	}
	return (out s)
}

# Links [text](url) and images ![alt](url).
function links(s,	out, i, j, k, text, url, img) {
	out = ""
	while ((i = index(s, "](")) > 0) {
		j = i
		while (j > 0 && substr(s, j, 1) != "[")
			j--
		k = index(substr(s, i + 2), ")")
		if (j == 0 || k == 0)
			break
		img = (j > 1 && substr(s, j - 1, 1) == "!")
		text = substr(s, j + 1, i - j - 1)
		url = substr(s, i + 2, k - 1)
		out = out substr(s, 1, j - 1 - img)
		if (img)
			out = out MUTED "[image: " text "]" FG
		else
			out = out ACCENT UNDER text NOUNDER FG MUTED " (" url ")" FG
		s = substr(s, i + 2 + k)
	}
	return (out s)
}

function inline(s,	parts, n, i, out, seg) {
	# Inline code first; its contents are not formatted further.
	n = split(s, parts, "`")
	out = ""
	for (i = 1; i <= n; i++) {
		seg = parts[i]
		if (i % 2 == 0 && i < n) {
			out = out CODE seg FG
			continue
		}
		seg = links(seg)
		seg = span(seg, "**", BOLD, NOBOLD)
		seg = span(seg, "__", BOLD, NOBOLD)
		seg = span(seg, "*", ITALIC, NOITALIC)
		seg = span(seg, "~~", DIM, NOBOLD)
		out = out seg
	}
	return (out)
}

# Fenced code blocks.
/^[ \t]*(```|~~~)/ {
	if (!incode) {
		lang = $0
		sub(/^[ \t]*(```|~~~)[ \t]*/, "", lang)
		print MUTED "  ╭─" (lang != "" ? " " lang " " : "") \
		    rule("─", 40) RESET
		incode = 1
	} else {
		print MUTED "  ╰─" rule("─", 40) RESET
		incode = 0
	}
	next
}
incode {
	print MUTED "  │ " RESET CODE $0 RESET
	next
}

# Headings.
/^#+[ \t]/ {
	level = match($0, /[^#]/) - 1
	text = $0
	sub(/^#+[ \t]+/, "", text)
	sub(/[ \t]+#+[ \t]*$/, "", text)
	if (level == 1) {
		print ""
		print BOLD ACCENT inline(text) RESET
		print ACCENT rule("━", WIDTH) RESET
	} else if (level == 2) {
		print ""
		print BOLD ACCENT2 inline(text) RESET
		print MUTED rule("─", WIDTH) RESET
	} else
		print BOLD inline(text) RESET
	next
}

# Setext underlines after a line (=== and ---) and horizontal rules.
/^[ \t]*(-[ \t]*-[ \t]*-[- \t]*|\*[ \t]*\*[ \t]*\*[* \t]*|_[ \t]*_[ \t]*_[_ \t]*)$/ {
	print MUTED rule("─", WIDTH) RESET
	next
}

# Block quotes.
/^[ \t]*>/ {
	text = $0
	sub(/^[ \t]*>[ \t]?/, "", text)
	print MUTED "  ▌ " RESET ITALIC inline(text) RESET
	next
}

# Task lists, bullet lists and numbered lists.
/^[ \t]*[-*+][ \t]+\[[ xX]\][ \t]/ {
	indent = $0; sub(/[^ \t].*/, "", indent)
	text = $0; sub(/^[ \t]*[-*+][ \t]+/, "", text)
	box = (substr(text, 2, 1) == " ") ? "☐" : "☑"
	sub(/^\[[ xX]\][ \t]+/, "", text)
	print indent "  " ACCENT box RESET " " inline(text)
	next
}
/^[ \t]*[-*+][ \t]+/ {
	indent = $0; sub(/[^ \t].*/, "", indent)
	text = $0; sub(/^[ \t]*[-*+][ \t]+/, "", text)
	print indent "  " ACCENT "•" RESET " " inline(text)
	next
}
/^[ \t]*[0-9]+[.)][ \t]+/ {
	indent = $0; sub(/[^ \t].*/, "", indent)
	num = $0; sub(/^[ \t]*/, "", num); sub(/[.)].*/, "", num)
	text = $0; sub(/^[ \t]*[0-9]+[.)][ \t]+/, "", text)
	print indent "  " ACCENT num "." RESET " " inline(text)
	next
}

# Indented code (four spaces or a tab, not inside a list).
/^(    |\t)/ {
	print MUTED "  │ " RESET CODE substr($0, ($0 ~ /^\t/) ? 2 : 5) RESET
	next
}

{
	print inline($0)
}
