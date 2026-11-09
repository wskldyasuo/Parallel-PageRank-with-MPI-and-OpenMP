#!/usr/bin/env python3
"""Build the Project 3 draft PDF from its Markdown source."""

from __future__ import annotations

import html
import hashlib
import re
import shutil
from pathlib import Path

from PIL import Image as PILImage
from matplotlib import font_manager, rcParams
from matplotlib.backends.backend_agg import FigureCanvasAgg
from matplotlib.figure import Figure
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import (
    Image,
    KeepTogether,
    ListFlowable,
    ListItem,
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)
from reportlab.platypus.tableofcontents import TableOfContents
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.pdfmetrics import EmbeddedType1Face, Font


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "report/project3_report_draft.md"
OUTPUT = ROOT / "output/pdf/DSA5208_Project3_Yan_Wan_Wang.pdf"
FONT_DIR = ROOT / "assets/fonts/latin-modern"
MATH_DIR = ROOT / "tmp/pdfs/report_math"


def register_latin_modern() -> None:
    """Register embedded Latin Modern Roman and Mono fonts for ReportLab."""
    fonts = {
        "LMRoman": "lmr10",
        "LMRoman-Bold": "lmbx10",
        "LMRoman-Italic": "lmri10",
        "LMRoman-BoldItalic": "lmbxi10",
        "LMMono": "lmtt10",
    }
    for alias, base in fonts.items():
        face = EmbeddedType1Face(str(FONT_DIR / f"{base}.afm"), str(FONT_DIR / f"{base}.pfb"))
        # ReportLab reads these decimal AFM values as strings; normalize them
        # so Paragraph line metrics work with the embedded Type 1 faces.
        face.ascent = float(face.ascent)
        face.descent = float(face.descent)
        pdfmetrics.registerTypeFace(face)
        pdfmetrics.registerFont(Font(alias, face.name, "WinAnsiEncoding"))
    pdfmetrics.registerFontFamily(
        "LMRoman",
        normal="LMRoman",
        bold="LMRoman-Bold",
        italic="LMRoman-Italic",
        boldItalic="LMRoman-BoldItalic",
    )
    pdfmetrics.registerFontFamily(
        "LMMono",
        normal="LMMono",
        bold="LMMono",
        italic="LMMono",
        boldItalic="LMMono",
    )


def configure_math_fonts() -> None:
    """Use Latin Modern faces for Matplotlib's LaTeX-style mathtext."""
    for font_file in FONT_DIR.glob("*.otf"):
        font_manager.fontManager.addfont(str(font_file))
    rcParams.update(
        {
            "font.family": "Latin Modern Roman",
            "mathtext.fontset": "custom",
            "mathtext.rm": "Latin Modern Roman",
            "mathtext.it": "Latin Modern Roman:italic",
            "mathtext.bf": "Latin Modern Roman:bold",
            "mathtext.fallback": "cm",
        }
    )


def render_math(latex: str, *, display: bool) -> tuple[Path, float, float]:
    """Render one LaTeX-style expression and return its path and size in points."""
    size = 12.5 if display else 10.3
    digest = hashlib.sha256(f"{display}:{size}:{latex}".encode("utf-8")).hexdigest()[:16]
    path = MATH_DIR / f"math-{digest}.png"
    if not path.exists():
        figure = Figure(figsize=(1, 1), dpi=300)
        FigureCanvasAgg(figure)
        figure.text(0, 0, f"${latex}$", fontsize=size, color="black")
        figure.savefig(path, dpi=300, transparent=True, bbox_inches="tight", pad_inches=0.015)
    with PILImage.open(path) as image:
        width = image.width / 300 * 72
        height = image.height / 300 * 72
    return path, width, height


def inline_markup(text: str) -> str:
    math_tokens: dict[str, str] = {}

    def replace_math(match: re.Match[str]) -> str:
        path, width, height = render_math(match.group(1), display=False)
        token = f"MATHINLINE{len(math_tokens)}TOKEN"
        math_tokens[token] = (
            f'<img src="{html.escape(str(path))}" width="{width:.2f}" '
            f'height="{height:.2f}" valign="-2"/>'
        )
        return token

    with_math_tokens = re.sub(r"\$([^$]+)\$", replace_math, text.strip())
    escaped = html.escape(with_math_tokens)
    escaped = re.sub(r"`([^`]+)`", r'<font name="LMMono">\1</font>', escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", escaped)
    for token, markup in math_tokens.items():
        escaped = escaped.replace(token, markup)
    return escaped


def page_decoration(canvas, document) -> None:
    canvas.saveState()
    if document.page > 2:
        canvas.setStrokeColor(colors.black)
        canvas.setLineWidth(0.45)
        canvas.line(2.0 * cm, A4[1] - 1.45 * cm, A4[0] - 2.0 * cm, A4[1] - 1.45 * cm)
        canvas.setFillColor(colors.black)
        canvas.setFont("LMRoman", 9)
        canvas.drawString(2.0 * cm, A4[1] - 1.18 * cm, "DSA5208 - Project 3")
        canvas.drawRightString(A4[0] - 2.0 * cm, A4[1] - 1.18 * cm, "Parallel PageRank")
        canvas.drawCentredString(A4[0] / 2, 0.82 * cm, str(document.page - 2))
    canvas.restoreState()


class ReportDocTemplate(SimpleDocTemplate):
    """Document template that registers headings for the generated TOC."""

    def afterFlowable(self, flowable) -> None:
        if not isinstance(flowable, Paragraph):
            return
        style_name = flowable.style.name
        if style_name not in {"H1Custom", "H2Custom"}:
            return
        title = flowable.getPlainText()
        if title in {"Abstract", "Contents"}:
            return
        level = 0 if style_name == "H1Custom" else 1
        slug = re.sub(r"[^A-Za-z0-9]+", "-", title).strip("-").lower()
        key = f"section-{level}-{slug}"
        self.canv.bookmarkPage(key)
        self.canv.addOutlineEntry(title, key, level=level, closed=False)
        self.notify("TOCEntry", (level, title, self.page - 2, key))


def parse_table(lines: list[str], styles) -> Table:
    rows = []
    for line in lines:
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if all(re.fullmatch(r":?-+:?", cell) for cell in cells):
            continue
        rows.append([Paragraph(inline_markup(cell), styles["TableCell"]) for cell in cells])

    column_count = max(len(row) for row in rows)
    usable = A4[0] - 4.0 * cm
    widths = [usable / column_count] * column_count
    table = Table(rows, colWidths=widths, repeatRows=1, hAlign="LEFT")
    table.setStyle(
        TableStyle(
            [
                ("TEXTCOLOR", (0, 0), (-1, -1), colors.black),
                ("FONTNAME", (0, 0), (-1, 0), "LMRoman"),
                ("LINEABOVE", (0, 0), (-1, 0), 0.7, colors.black),
                ("LINEBELOW", (0, 0), (-1, 0), 0.45, colors.black),
                ("LINEBELOW", (0, -1), (-1, -1), 0.7, colors.black),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 3),
                ("RIGHTPADDING", (0, 0), (-1, -1), 3),
                ("TOPPADDING", (0, 0), (-1, -1), 3),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
            ]
        )
    )
    return table


def build() -> None:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    if MATH_DIR.exists():
        shutil.rmtree(MATH_DIR)
    MATH_DIR.mkdir(parents=True, exist_ok=True)
    register_latin_modern()
    configure_math_fonts()
    styles = getSampleStyleSheet()
    styles.add(
        ParagraphStyle(
            name="ReportTitle",
            parent=styles["Title"],
            fontName="LMRoman",
            fontSize=17,
            leading=21,
            textColor=colors.black,
            alignment=TA_CENTER,
            spaceAfter=24,
        )
    )
    styles.add(ParagraphStyle(name="Institution", fontName="LMRoman", fontSize=14, leading=19, alignment=TA_CENTER, textColor=colors.black))
    styles.add(ParagraphStyle(name="Course", fontName="LMRoman", fontSize=12, leading=17, alignment=TA_CENTER, textColor=colors.black))
    styles.add(ParagraphStyle(name="ProjectLabel", fontName="LMRoman-Bold", fontSize=20, leading=25, alignment=TA_CENTER, textColor=colors.black, spaceBefore=24, spaceAfter=15))
    styles.add(ParagraphStyle(name="AbstractHeading", fontName="LMRoman-Bold", fontSize=11.5, leading=14, alignment=TA_CENTER, textColor=colors.black, spaceAfter=6))
    styles.add(ParagraphStyle(name="ContentsHeading", fontName="LMRoman-Bold", fontSize=17, leading=20, textColor=colors.black, spaceAfter=10))
    styles.add(
        ParagraphStyle(
            name="H1Custom",
            parent=styles["Heading1"],
            fontName="LMRoman-Bold",
            fontSize=15,
            leading=19,
            textColor=colors.black,
            spaceBefore=13,
            spaceAfter=7,
            keepWithNext=True,
        )
    )
    styles.add(
        ParagraphStyle(
            name="H2Custom",
            parent=styles["Heading2"],
            fontName="LMRoman-Bold",
            fontSize=11.8,
            leading=15,
            textColor=colors.black,
            spaceBefore=9,
            spaceAfter=5,
            keepWithNext=True,
        )
    )
    styles.add(
        ParagraphStyle(
            name="BodyCustom",
            parent=styles["BodyText"],
            fontName="LMRoman",
            fontSize=10.1,
            leading=13.0,
            textColor=colors.black,
            spaceAfter=6,
            alignment=4,
        )
    )
    styles.add(
        ParagraphStyle(
            name="Reference",
            parent=styles["BodyCustom"],
            fontSize=9.2,
            leading=11.2,
            spaceAfter=4,
        )
    )
    styles.add(
        ParagraphStyle(
            name="TableCell",
            parent=styles["BodyText"],
            fontName="LMRoman",
            fontSize=9,
            leading=10.8,
            textColor=colors.black,
        )
    )
    styles.add(
        ParagraphStyle(
            name="Caption",
            parent=styles["BodyText"],
            fontName="LMRoman",
            fontSize=9.3,
            leading=11.2,
            alignment=TA_CENTER,
            textColor=colors.black,
            spaceBefore=3,
            spaceAfter=9,
        )
    )
    styles.add(
        ParagraphStyle(
            name="TableCaption",
            parent=styles["BodyText"],
            fontName="LMRoman",
            fontSize=9.3,
            leading=11.2,
            alignment=TA_CENTER,
            textColor=colors.black,
            spaceBefore=6,
            spaceAfter=4,
            keepWithNext=True,
        )
    )
    styles.add(
        ParagraphStyle(
            name="TitleMeta",
            parent=styles["BodyText"],
            fontName="LMRoman",
            fontSize=11,
            leading=15,
            alignment=TA_CENTER,
            textColor=colors.black,
        )
    )
    toc_level_styles = [
        ParagraphStyle(
            name="TOCLevel1",
            fontName="LMRoman-Bold",
            fontSize=11,
            leading=15.5,
            leftIndent=0,
            firstLineIndent=0,
            textColor=colors.HexColor("#183B6B"),
            spaceBefore=4,
        ),
        ParagraphStyle(
            name="TOCLevel2",
            fontName="LMRoman",
            fontSize=10,
            leading=13,
            leftIndent=18,
            firstLineIndent=0,
            textColor=colors.HexColor("#183B6B"),
        ),
    ]

    lines = SOURCE.read_text(encoding="utf-8").splitlines()
    story = []
    paragraph_buffer: list[str] = []
    bullet_buffer: list[str] = []
    title_done = False
    figure_number = 0
    table_number = 0
    in_references = False

    def flush_paragraph() -> None:
        if paragraph_buffer:
            paragraph_style = styles["Reference"] if in_references else styles["BodyCustom"]
            story.append(Paragraph(inline_markup(" ".join(paragraph_buffer)), paragraph_style))
            paragraph_buffer.clear()

    def flush_bullets() -> None:
        if bullet_buffer:
            items = [ListItem(Paragraph(inline_markup(x), styles["BodyCustom"]), leftIndent=9) for x in bullet_buffer]
            story.append(ListFlowable(items, bulletType="bullet", start="circle", leftIndent=18, bulletFontSize=6))
            story.append(Spacer(1, 3))
            bullet_buffer.clear()

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        display_math = re.fullmatch(r"\$\$(.+)\$\$", stripped)
        if display_math:
            flush_paragraph()
            flush_bullets()
            path, width, height = render_math(display_math.group(1), display=True)
            equation = Image(str(path), width=width, height=height)
            equation.hAlign = "CENTER"
            story.append(equation)
            story.append(Spacer(1, 7))
            i += 1
            continue

        if stripped.startswith("|"):
            flush_paragraph()
            flush_bullets()
            block = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                block.append(lines[i])
                i += 1
            story.append(parse_table(block, styles))
            story.append(Spacer(1, 8))
            continue

        table_caption_match = re.fullmatch(r"Table:\s*(.+)", stripped)
        if table_caption_match:
            flush_paragraph()
            flush_bullets()
            table_number += 1
            table_caption = f"Table {table_number}: {table_caption_match.group(1)}"
            story.append(Paragraph(inline_markup(table_caption), styles["TableCaption"]))
            i += 1
            continue

        image_match = re.fullmatch(r"!\[(.+)]\((.+)\)", stripped)
        if image_match:
            flush_paragraph()
            flush_bullets()
            caption, relative = image_match.groups()
            figure_number += 1
            path = ROOT / relative
            if not path.exists():
                raise FileNotFoundError(path)
            image = Image(str(path))
            max_width = A4[0] - 4.0 * cm
            max_height = 11.0 * cm
            scale = min(max_width / image.imageWidth, max_height / image.imageHeight)
            image.drawWidth = image.imageWidth * scale
            image.drawHeight = image.imageHeight * scale
            figure_caption = f"Figure {figure_number}: {caption}"
            story.append(KeepTogether([image, Paragraph(inline_markup(figure_caption), styles["Caption"])]))
            i += 1
            continue

        if stripped.startswith("# "):
            flush_paragraph()
            flush_bullets()
            if not title_done:
                story.append(Spacer(1, 1.7 * cm))
                story.append(Paragraph("National University of Singapore", styles["Institution"]))
                story.append(Spacer(1, 0.35 * cm))
                story.append(Paragraph("DSA5208 Scalable Distributed Computing for Data Science", styles["Course"]))
                story.append(Paragraph("Project 3 Report", styles["ProjectLabel"]))
                story.append(Paragraph(inline_markup(stripped[2:]), styles["ReportTitle"]))
                title_done = True
            else:
                story.append(Paragraph(inline_markup(stripped[2:]), styles["H1Custom"]))
            i += 1
            continue

        if stripped.startswith("## "):
            flush_paragraph()
            flush_bullets()
            heading = stripped[3:]
            in_references = heading == "References"
            if heading == "Abstract":
                # Match the reference cover: place the abstract block near the
                # bottom of page 1 rather than immediately below the metadata.
                story.append(Spacer(1, 10.9 * cm))
                story.append(Paragraph(heading, styles["AbstractHeading"]))
            elif heading == "Contents":
                story.append(PageBreak())
                story.append(Paragraph(heading, styles["ContentsHeading"]))
                toc = TableOfContents()
                toc.levelStyles = toc_level_styles
                toc.dotsMinLevel = 0
                story.append(toc)
                story.append(PageBreak())
            else:
                story.append(Paragraph(inline_markup(heading), styles["H1Custom"]))
            i += 1
            continue

        if stripped.startswith("### "):
            flush_paragraph()
            flush_bullets()
            story.append(Paragraph(inline_markup(stripped[4:]), styles["H2Custom"]))
            i += 1
            continue

        if stripped.startswith("- "):
            flush_paragraph()
            bullet_buffer.append(stripped[2:])
            i += 1
            continue

        if not stripped:
            flush_paragraph()
            flush_bullets()
            i += 1
            continue

        if title_done and stripped.startswith("**"):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped), styles["TitleMeta"]))
            i += 1
            continue

        paragraph_buffer.append(stripped)
        i += 1

    flush_paragraph()
    flush_bullets()

    document = ReportDocTemplate(
        str(OUTPUT),
        pagesize=A4,
        rightMargin=2.0 * cm,
        leftMargin=2.0 * cm,
        topMargin=1.8 * cm,
        bottomMargin=1.7 * cm,
        title="Parallel PageRank with MPI and OpenMP",
        author="Yan Tianao, Wan Gehong, Wang Tianyi",
        subject="DSA5208 Project 3",
    )
    document.multiBuild(story, onFirstPage=page_decoration, onLaterPages=page_decoration)
    shutil.rmtree(MATH_DIR)
    print(OUTPUT.relative_to(ROOT))


if __name__ == "__main__":
    build()
