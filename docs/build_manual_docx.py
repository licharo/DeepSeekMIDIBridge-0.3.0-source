"""Builds docs/使用手册.docx from docs/使用手册.md.

The manual is maintained as Markdown; this script renders it as a Word document
with proper heading styles, tables, lists and code blocks (CJK-safe fonts).
"""

import os
import re
import sys
from pathlib import Path

from docx import Document
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Pt, RGBColor, Cm

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "使用手册.md"
TARGET = ROOT / "docs" / "使用手册.docx"

# DMB_MANUAL_FONT=none keeps the document's own default font instead
_font = os.environ.get("DMB_MANUAL_FONT", "Microsoft YaHei").strip()
BODY_FONT = None if _font.lower() in ("", "none") else _font
MONO_FONT = "Consolas"
ACCENT = RGBColor(0x1F, 0x5F, 0xA8)
DIM = RGBColor(0x55, 0x55, 0x55)


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def set_style_fonts(document, name, latin, east_asian, size=None):
    style = document.styles[name]
    if latin is not None:
        style.font.name = latin
    if size is not None:
        style.font.size = Pt(size)

    if latin is None:
        return

    rpr = style.element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.append(rfonts)
    rfonts.set(qn("w:ascii"), latin)
    rfonts.set(qn("w:hAnsi"), latin)
    rfonts.set(qn("w:eastAsia"), east_asian)

    # "Allow Line Breaking At Character Level": otherwise a long path such as
    # C:\Program Files\Common Files\VST3 is one unbreakable word and the line runs
    # past the right margin in viewers that ignore the table/paragraph defaults
    ppr = style.element.get_or_add_pPr()
    for existing in ppr.findall(qn("w:wordWrap")):
        ppr.remove(existing)
    word_wrap = OxmlElement("w:wordWrap")
    word_wrap.set(qn("w:val"), "true")
    ppr.append(word_wrap)


def shade(element, colour):
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), colour)
    element.append(shd)


def set_run_fonts(run, latin, east_asian):
    """Every run declares its East Asian font, otherwise LibreOffice/Word may treat
    CJK text as one unbreakable Latin word and let the line run past the margin."""
    if latin is None:
        return

    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.insert(0, rfonts)
    rfonts.set(qn("w:ascii"), latin)
    rfonts.set(qn("w:hAnsi"), latin)
    rfonts.set(qn("w:eastAsia"), east_asian or latin)
    rfonts.set(qn("w:cs"), latin)


def add_runs(paragraph, text, base_size=None, base_colour=None):
    """Adds text to a paragraph, honouring **bold** and `code` spans."""
    pattern = re.compile(r"(\*\*.+?\*\*|`[^`]+`|\[[^\]]+\]\([^)]*\))")
    for chunk in pattern.split(text):
        if not chunk:
            continue
        if chunk.startswith("**") and chunk.endswith("**"):
            run = paragraph.add_run(chunk[2:-2])
            run.bold = True
            set_run_fonts(run, BODY_FONT, BODY_FONT)
        elif chunk.startswith("`") and chunk.endswith("`"):
            run = paragraph.add_run(chunk[1:-1])
            run.font.name = MONO_FONT
            run.font.size = Pt((base_size or 10.5) - 0.5)
            set_run_fonts(run, MONO_FONT, BODY_FONT)
        elif chunk.startswith("[") and "](" in chunk:
            run = paragraph.add_run(chunk[1:chunk.index("]")])
            set_run_fonts(run, BODY_FONT, BODY_FONT)
        else:
            run = paragraph.add_run(chunk)
            set_run_fonts(run, BODY_FONT, BODY_FONT)

        if base_size is not None:
            run.font.size = Pt(base_size)
        if base_colour is not None:
            run.font.color.rgb = base_colour


def add_code_block(document, lines):
    paragraph = document.add_paragraph()
    paragraph.paragraph_format.left_indent = Cm(0.4)
    paragraph.paragraph_format.space_before = Pt(4)
    paragraph.paragraph_format.space_after = Pt(8)
    shade(paragraph._p.get_or_add_pPr(), "F2F4F7")
    for index, line in enumerate(lines):
        run = paragraph.add_run(line)
        run.font.name = MONO_FONT
        run.font.size = Pt(9)
        set_run_fonts(run, MONO_FONT, BODY_FONT)
        if index != len(lines) - 1:
            run.add_break()


def add_table(document, rows):
    """Tables are rendered as "bold term + indented description" blocks.

    A real Word table would need the consumer to honour fixed column widths; the
    bundled LibreOffice renderer stretches a column as soon as a cell holds a long
    unbreakable path, which pushes the table past the right margin. Plain
    paragraphs wrap reliably everywhere, so the manual uses this layout instead.
    """
    header, *body = rows

    for row in body:
        term = row[0]
        details = row[1:]

        paragraph = document.add_paragraph()
        paragraph.paragraph_format.left_indent = Cm(0.75)
        paragraph.paragraph_format.first_line_indent = Cm(-0.75)
        paragraph.paragraph_format.space_before = Pt(4)
        paragraph.paragraph_format.space_after = Pt(0)
        add_runs(paragraph, "**" + term + "**" if not term.startswith("**") else term)

        for index, text in enumerate(details):
            if not text:
                continue

            detail = document.add_paragraph()
            detail.paragraph_format.left_indent = Cm(0.75)
            detail.paragraph_format.space_before = Pt(0)
            detail.paragraph_format.space_after = Pt(0)

            label = ""
            if len(header) > 2 and index + 1 < len(header) and header[index + 1] != term:
                label = "**" + header[index + 1] + "：** "

            add_runs(detail, label + text)

    document.add_paragraph()


def split_row(line):
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


# --------------------------------------------------------------------------- #
# conversion
# --------------------------------------------------------------------------- #
def build():
    text = SOURCE.read_text(encoding="utf-8")
    lines = text.splitlines()

    document = Document()
    set_style_fonts(document, "Normal", BODY_FONT, BODY_FONT, 10.5)
    for name in ("Heading 1", "Heading 2", "Heading 3", "Heading 4"):
        set_style_fonts(document, name, BODY_FONT, BODY_FONT)
    for name in ("List Bullet", "List Number"):
        set_style_fonts(document, name, BODY_FONT, BODY_FONT, 10.5)

    for section in document.sections:
        section.left_margin = Cm(2.0)
        section.right_margin = Cm(2.0)
        section.top_margin = Cm(2.0)
        section.bottom_margin = Cm(2.0)

    index = 0
    in_code = False
    code_lines = []
    first_heading_done = False

    while index < len(lines):
        line = lines[index]
        stripped = line.strip()

        # ---- code fence ---------------------------------------------------- #
        if stripped.startswith("```"):
            if in_code:
                add_code_block(document, code_lines)
                code_lines = []
                in_code = False
            else:
                in_code = True
            index += 1
            continue

        if in_code:
            code_lines.append(line.rstrip())
            index += 1
            continue

        # ---- table --------------------------------------------------------- #
        if stripped.startswith("|") and index + 1 < len(lines) and set(lines[index + 1].strip()) <= set("|-: "):
            rows = [split_row(stripped)]
            index += 2
            while index < len(lines) and lines[index].strip().startswith("|"):
                rows.append(split_row(lines[index].strip()))
                index += 1
            add_table(document, rows)
            continue

        # ---- headings ------------------------------------------------------ #
        if stripped.startswith("#"):
            level = len(stripped) - len(stripped.lstrip("#"))
            title = stripped[level:].strip()
            if level == 1 and not first_heading_done:
                heading = document.add_heading(title, level=0)
                first_heading_done = True
            else:
                heading = document.add_heading(title, level=min(level, 4))
            for run in heading.runs:
                run.font.color.rgb = ACCENT
            index += 1
            continue

        # ---- horizontal rule ----------------------------------------------- #
        if stripped in ("---", "***", "___"):
            paragraph = document.add_paragraph()
            paragraph.paragraph_format.space_after = Pt(2)
            ppr = paragraph._p.get_or_add_pPr()
            borders = OxmlElement("w:pBdr")
            bottom = OxmlElement("w:bottom")
            bottom.set(qn("w:val"), "single")
            bottom.set(qn("w:sz"), "6")
            bottom.set(qn("w:color"), "C9CDD4")
            borders.append(bottom)
            ppr.append(borders)
            index += 1
            continue

        # ---- blockquote ---------------------------------------------------- #
        if stripped.startswith(">"):
            paragraph = document.add_paragraph(style="Intense Quote")
            add_runs(paragraph, stripped.lstrip("> ").strip())
            index += 1
            continue

        # ---- lists --------------------------------------------------------- #
        if re.match(r"^[-*]\s+", stripped):
            paragraph = document.add_paragraph(style="List Bullet")
            add_runs(paragraph, re.sub(r"^[-*]\s+", "", stripped))
            index += 1
            continue

        if re.match(r"^\d+\.\s+", stripped):
            # plain paragraphs with the literal number: Word's List Number style
            # would continue one numbering sequence across the whole document
            paragraph = document.add_paragraph()
            paragraph.paragraph_format.left_indent = Cm(0.75)
            paragraph.paragraph_format.first_line_indent = Cm(-0.75)
            add_runs(paragraph, stripped)
            index += 1
            continue

        # ---- image --------------------------------------------------------- #
        image = re.match(r"^!\[[^\]]*\]\(([^)]+)\)", stripped)
        if image:
            candidate = Path(image.group(1))
            if not candidate.is_absolute():
                candidate = ROOT / candidate
            if candidate.exists():
                document.add_picture(str(candidate), width=Cm(16))
            index += 1
            continue

        # ---- blank / paragraph --------------------------------------------- #
        if not stripped:
            index += 1
            continue

        paragraph = document.add_paragraph()
        add_runs(paragraph, stripped)
        index += 1

    if code_lines:
        add_code_block(document, code_lines)

    document.save(TARGET)
    print(f"wrote {TARGET}")


if __name__ == "__main__":
    build()
