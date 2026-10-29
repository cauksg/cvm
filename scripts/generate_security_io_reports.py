#!/usr/bin/env python3
"""Generate the short and detailed security I/O verification reports as DOCX."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile
from xml.sax.saxutils import escape, quoteattr
from datetime import date
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "reports"
IMAGE = ROOT / "docs" / "figures" / "security-io-architecture.png"
OUT.mkdir(parents=True, exist_ok=True)

NS_W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
NS_R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
NS_WP = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
NS_A = "http://schemas.openxmlformats.org/drawingml/2006/main"
NS_PIC = "http://schemas.openxmlformats.org/drawingml/2006/picture"

def X(tag, attrs="", body=""):
    return f"<{tag}{attrs}>{body}</{tag.split()[-1]}>"

def w(tag, attrs="", body=""):
    return X("w:" + tag, attrs, body)

def r(tag, attrs="", body=""):
    # Run-level elements are WordprocessingML elements (w:r, w:t, w:br, ...).
    return X("w:" + tag, attrs, body)

def esc(s):
    return escape(str(s), {'"': '&quot;'})

def run(text, bold=False, italic=False, color=None, size=None):
    props = []
    if bold: props.append(w("b"))
    if italic: props.append(w("i"))
    if color: props.append(w("color", f' w:val="{color}"'))
    if size: props.append(w("sz", f' w:val="{size}"'))
    rp = w("rPr", "", "".join(props)) if props else ""
    # Preserve line breaks in report paragraphs.
    parts = str(text).split("\n")
    out = []
    for i, part in enumerate(parts):
        out.append(r("t", ' xml:space="preserve"', esc(part)))
        if i != len(parts) - 1:
            out.append(r("br"))
    return r("r", "", rp + "".join(out))

def para(text="", style=None, align=None, before=None, after=None, keep=False):
    props = []
    if style: props.append(w("pStyle", f' w:val="{style}"'))
    if align: props.append(w("jc", f' w:val="{align}"'))
    if before is not None or after is not None:
        attrs = ""
        if before is not None: attrs += f' w:before="{before}"'
        if after is not None: attrs += f' w:after="{after}"'
        props.append(w("spacing", attrs))
    if keep: props.append(w("keepNext"))
    pp = w("pPr", "", "".join(props)) if props else ""
    return w("p", "", pp + run(text))

def rich_para(parts, style=None, align=None, before=None, after=None, keep=False):
    props = []
    if style: props.append(w("pStyle", f' w:val="{style}"'))
    if align: props.append(w("jc", f' w:val="{align}"'))
    if before is not None or after is not None:
        attrs = ""
        if before is not None: attrs += f' w:before="{before}"'
        if after is not None: attrs += f' w:after="{after}"'
        props.append(w("spacing", attrs))
    if keep: props.append(w("keepNext"))
    pp = w("pPr", "", "".join(props)) if props else ""
    body = "".join(run(t, bold=b, italic=i, color=c, size=s) for t,b,i,c,s in parts)
    return w("p", "", pp + body)

def bullet(text, level=0):
    return w("p", "", w("pPr", "", w("numPr", "", w("ilvl", f' w:val="{level}"') + w("numId", ' w:val="1"'))) + run(text))

def number(text):
    return w("p", "", w("pPr", "", w("numPr", "", w("ilvl", ' w:val="0"') + w("numId", ' w:val="2"'))) + run(text))

def page_break():
    return w("p", "", w("r", "", r("br", ' w:type="page"')))

def heading(level, text):
    return para(text, f"Heading{level}", before=260 if level == 1 else 180, after=100, keep=True)

def cell(text, header=False, width=1800, align="left"):
    shd = w("shd", ' w:fill="1F4E78"' if header else ' w:fill="F3F6F9"')
    color = "FFFFFF" if header else "172B46"
    tcpr = w("tcPr", "", w("tcW", f' w:w="{width}" w:type="dxa"') + shd + w("tcMar", "", w("top", ' w:w="90" w:type="dxa"') + w("start", ' w:w="110" w:type="dxa"') + w("bottom", ' w:w="90" w:type="dxa"') + w("end", ' w:w="110" w:type="dxa"')))
    ppr = w("pPr", "", w("jc", f' w:val="{align}"'))
    return w("tc", "", tcpr + w("p", "", ppr + run(text, bold=header, color=color, size=20 if not header else 19)))

def table(headers, rows, widths=None):
    widths = widths or [1800] * len(headers)
    borders = w("tblBorders", "", "".join(w(edge, ' w:val="single" w:sz="6" w:space="0" w:color="B8C6D3"') for edge in ("top", "left", "bottom", "right", "insideH", "insideV")))
    layout = w("tblLayout", ' w:type="fixed"')
    grid = w("tblGrid", "", "".join(w("gridCol", f' w:w="{v}"') for v in widths))
    tblpr = w("tblPr", "", w("tblW", ' w:w="0" w:type="auto"') + layout + borders + w("tblCellMar", "", w("top", ' w:w="80" w:type="dxa"') + w("start", ' w:w="100" w:type="dxa"') + w("bottom", ' w:w="80" w:type="dxa"') + w("end", ' w:w="100" w:type="dxa"')))
    head = w("tr", "", "".join(cell(x, True, widths[i]) for i,x in enumerate(headers)))
    body = []
    for row in rows:
        body.append(w("tr", "", "".join(cell(x, False, widths[i]) for i,x in enumerate(row))))
    return w("tbl", "", tblpr + grid + head + "".join(body))

def image_paragraph(rel_id="rIdImage", width_in=6.65, filename="security-io-architecture.png"):
    # Original image is 4400x2960, so preserve its aspect ratio.
    cx = int(width_in * 914400)
    cy = int(cx * 2960 / 4400)
    drawing = (
        f'<wp:inline xmlns:wp="{NS_WP}" distT="0" distB="0" distL="0" distR="0">'
        f'<wp:extent cx="{cx}" cy="{cy}"/>'
        f'<wp:docPr id="1" name="安全 I/O 通路架构图" descr="安全 I/O 通路架构图"/>'
        f'<a:graphic xmlns:a="{NS_A}"><a:graphicData uri="{NS_PIC}">'
        f'<pic:pic xmlns:pic="{NS_PIC}">'
        '<pic:nvPicPr><pic:cNvPr id="0" name="security-io-architecture.png"/><pic:cNvPicPr/></pic:nvPicPr>'
        '<pic:blipFill><a:blip r:embed="' + rel_id + '" xmlns:r="' + NS_R + '"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
        '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="' + str(cx) + '" cy="' + str(cy) + '"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>'
        '</pic:pic></a:graphicData></a:graphic></wp:inline>'
    )
    return w("p", "", w("pPr", "", w("jc", ' w:val="center"')) + w("r", "", w("drawing", "", drawing)))

def toc():
    field = r("fldChar", ' w:fldCharType="begin"') + r("instrText", ' xml:space="preserve"', ' TOC \\o "1-3" \\h \\z \\u ') + r("fldChar", ' w:fldCharType="separate"') + r("t", "", "目录将在 Word 中更新") + r("fldChar", ' w:fldCharType="end"')
    return w("p", "", w("pPr", "", w("jc", ' w:val="left"')) + w("r", "", field))

def document_xml(body):
    sect = w("sectPr", "", w("pgSz", ' w:w="11906" w:h="16838"') + w("pgMar", ' w:top="1200" w:right="1250" w:bottom="1200" w:left="1250" w:header="600" w:footer="600" w:gutter="0"') + w("headerReference", ' w:type="default" r:id="rIdHeader"') + w("footerReference", ' w:type="default" r:id="rIdFooter"'))
    return f'<w:document xmlns:w="{NS_W}" xmlns:r="{NS_R}"><w:body>{body}{sect}</w:body></w:document>'

def styles_xml():
    styles = w("docDefaults", "", w("rPrDefault", "", w("rPr", "", w("rFonts", ' w:ascii="Arial" w:hAnsi="Arial" w:eastAsia="Microsoft YaHei"') + w("sz", ' w:val="21"') + w("lang", ' w:val="zh-CN" w:eastAsia="zh-CN"'))) + w("pPrDefault", "", w("pPr", "", w("spacing", ' w:line="320" w:lineRule="auto" w:after="90"'))))
    defs = []
    for name, size, color, boldv, space in [
        ("Title", 42, "172B46", True, 120), ("Subtitle", 25, "5D6D82", False, 120),
        ("Heading1", 31, "1F4E78", True, 180), ("Heading2", 25, "2F6690", True, 120),
        ("Heading3", 22, "395C92", True, 90), ("Caption", 18, "5D6D82", False, 80),
    ]:
        defs.append(w("style", f' w:type="paragraph" w:styleId="{name}"', w("name", f' w:val="{name}"') + w("basedOn", ' w:val="Normal"') + w("next", ' w:val="Normal"') + w("rPr", "", w("rFonts", ' w:ascii="Arial" w:hAnsi="Arial" w:eastAsia="Microsoft YaHei"') + w("sz", f' w:val="{size}"') + w("color", f' w:val="{color}"') + (w("b") if boldv else "")) + w("pPr", "", w("spacing", f' w:before="{space}" w:after="{space}" w:line="340"'))))
    return f'<w:styles xmlns:w="{NS_W}">' + w("style", ' w:type="paragraph" w:default="1" w:styleId="Normal"', w("name", ' w:val="Normal"') + w("rPr", "", w("rFonts", ' w:ascii="Arial" w:hAnsi="Arial" w:eastAsia="Microsoft YaHei"') + w("sz", ' w:val="21"'))) + "".join(defs) + w("style", ' w:type="paragraph" w:styleId="ListParagraph"', w("name", ' w:val="List Paragraph"') + w("basedOn", ' w:val="Normal"') + w("pPr", "", w("ind", ' w:left="480" w:hanging="240"'))) + '</w:styles>'

def numbering_xml():
    abstract = w("abstractNum", ' w:abstractNumId="0"', w("multiLevelType", ' w:val="hybridMultilevel"') + w("lvl", ' w:ilvl="0"', w("start", ' w:val="1"') + w("numFmt", ' w:val="bullet"') + w("lvlText", ' w:val="•"') + w("pPr", "", w("ind", ' w:left="480" w:hanging="240"'))))
    decimal = w("abstractNum", ' w:abstractNumId="1"', w("multiLevelType", ' w:val="singleLevel"') + w("lvl", ' w:ilvl="0"', w("start", ' w:val="1"') + w("numFmt", ' w:val="decimal"') + w("lvlText", ' w:val="%1."') + w("pPr", "", w("ind", ' w:left="480" w:hanging="240"'))))
    return f'<w:numbering xmlns:w="{NS_W}">{abstract}{decimal}{w("num", ' w:numId="1"', w("abstractNumId", ' w:val="0"'))}{w("num", ' w:numId="2"', w("abstractNumId", ' w:val="1"'))}</w:numbering>'

def header_xml():
    return f'<w:hdr xmlns:w="{NS_W}" xmlns:r="{NS_R}"><w:p><w:pPr><w:jc w:val="right"/></w:pPr><w:r><w:rPr><w:color w:val="5D6D82"/><w:sz w:val="16"/></w:rPr><w:t>xs-cvm · 安全 I/O 通路验证报告</w:t></w:r></w:p></w:hdr>'

def footer_xml():
    fld = r("fldChar", ' w:fldCharType="begin"') + r("instrText", ' xml:space="preserve"', " PAGE ") + r("fldChar", ' w:fldCharType="end"')
    return f'<w:ftr xmlns:w="{NS_W}" xmlns:r="{NS_R}"><w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:rPr><w:color w:val="5D6D82"/><w:sz w:val="16"/></w:rPr><w:t>第 </w:t></w:r><w:r>{fld}</w:r><w:r><w:rPr><w:color w:val="5D6D82"/><w:sz w:val="16"/></w:rPr><w:t> 页</w:t></w:r></w:p></w:ftr>'

def rels_xml():
    return f'''<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rIdHeader" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/header" Target="header1.xml"/>
<Relationship Id="rIdFooter" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>
<Relationship Id="rIdStyles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
<Relationship Id="rIdNumbering" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering" Target="numbering.xml"/>
<Relationship Id="rIdImage" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/security-io-architecture.png"/>
</Relationships>'''

def root_rels():
    return '''<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>'''

def content_types():
    return '''<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Default Extension="png" ContentType="image/png"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/word/numbering.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml"/>
<Override PartName="/word/header1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"/>
<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>'''

def core_props(title):
    return f'''<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:dcmitype="http://purl.org/dc/dcmitype/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>{escape(title)}</dc:title><dc:subject>虚拟机可信执行环境安全 I/O 通路原型验证</dc:subject><dc:creator>xs-cvm</dc:creator><cp:keywords>安全 I/O, 机密虚拟机, CoVE-IO, 验证报告</cp:keywords><dcterms:created xsi:type="dcterms:W3CDTF">2026-09-22T00:00:00Z</dcterms:created></cp:coreProperties>'''

def app_props():
    return '<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>xs-cvm report generator</Application><AppVersion>1.0</AppVersion></Properties>'

def package_docx(path, title, body):
    files = {
        '[Content_Types].xml': content_types(),
        '_rels/.rels': root_rels(),
        'word/document.xml': document_xml(body),
        'word/styles.xml': styles_xml(),
        'word/numbering.xml': numbering_xml(),
        'word/header1.xml': header_xml(),
        'word/footer1.xml': footer_xml(),
        'word/_rels/document.xml.rels': rels_xml(),
        'docProps/core.xml': core_props(title),
        'docProps/app.xml': app_props(),
    }
    with ZipFile(path, 'w', ZIP_DEFLATED) as z:
        for name, value in files.items():
            z.writestr(name, value.encode('utf-8'))
        z.write(IMAGE, 'word/media/security-io-architecture.png')

def common_cover(title, subtitle):
    return (
        para("", after=260) + para(title, "Title", "center", after=220) +
        para(subtitle, "Subtitle", "center", after=300) +
        para("验证对象：xs-cvm 虚拟机可信执行环境安全 I/O 通路原型验证系统", align="center", after=90) +
        para("验证日期：2026年9月22日", align="center", after=90) +
        para("结论：原型验证范围内通过", align="center", after=360) +
        image_paragraph() + para("图1  安全 I/O 通路架构", "Caption", "center", after=260) + page_break() +
        heading(1, "目录") + toc() + page_break()
    )

def short_body():
    b = common_cover("安全 I/O 通路原型验证报告", "机密虚拟机与典型 I/O 设备通路保护符合性评估")
    b += heading(1, "1. 验证目的")
    b += para("本报告评估系统是否支持机密虚拟机与典型 I/O 设备之间的受控连接，以及对 MMIO、DMA、中断和设备生命周期实施通路保护。报告结论限定在原型验证层面，不将机制验证扩展为产品级安全认证。")
    b += heading(1, "2. 结论摘要")
    b += rich_para([("验证结论：", True, False, "1F4E78", 22), ("通过。", True, False, "287B55", 22), ("现有实现已经形成从设备绑定、Guest 接受、运行期授权到 STOP/解绑回收的安全 I/O 原型闭环。", False, False, None, 22)])
    b += table(["要求", "实现与验证依据", "判定"], [
        ("机密虚拟机支持", "CVM 创建、vCPU 管理、内存所有权、受保护页表和生命周期处理。", "满足"),
        ("典型 I/O 设备支持", "VFIO PCI 设备绑定，覆盖单设备和多设备场景。", "满足"),
        ("设备连接控制", "TDI 依次经历注册、配置、绑定和 Guest 接受；未接受接口不能进入运行态。", "满足"),
        ("MMIO 保护", "MMIO 范围、owner、接口状态和 generation 参与运行态查询。", "满足"),
        ("DMA 保护", "PCI RID、IOMMU group、CVM owner、IOVA/GPA 范围和 IOMMU 映射共同约束。", "满足"),
        ("中断保护", "GSI、设备身份、目标 vCPU、IMSIC IID 和 MSI/MRIF 映射共同约束。", "满足"),
        ("停止与解绑", "STOP 后撤销 DMA、MMIO、IRQ 和设备映射，随后回收 TDI。", "满足"),
        ("可重复验证", "具备 COVG、VFIO、MSI、PRI、STOP、生命周期、资源回滚测试入口。", "满足"),
    ], [2400, 5500, 900])
    b += heading(1, "3. 安全 I/O 通路")
    b += para("系统将安全 I/O 通路拆分为管理授权、MMIO、DMA 和中断四类路径。CVM Monitor 管理设备归属、访问范围、接口状态和 generation；Host KVM/VFIO 负责设备绑定及映射配置；IOMMU 负责 DMA 地址和中断重映射约束；Guest 通过 COVG 检查报告并接受设备。")
    b += table(["通路", "保护对象", "主要判据"], [
        ("管理授权", "TDI 与 CVM 的关系", "owner、状态、generation、设备身份"),
        ("MMIO", "设备寄存器访问", "运行态、MMIO 范围、owner 匹配"),
        ("DMA", "设备到 Guest 内存的数据传输", "PCI RID、IOMMU group、IOVA/GPA 范围、映射状态"),
        ("中断", "MSI/MSI-X 到 Guest vCPU 的投递", "设备身份、GSI、目标 vCPU、IMSIC IID、MRIF 状态"),
    ], [1600, 2900, 4300])
    b += heading(1, "4. 验证结果")
    b += table(["场景", "验证结果"], [
        ("COVG 接受和停止", "通过；Guest 可枚举、检查接口报告并接受设备，STOP 后运行态访问被撤销。"),
        ("单 VFIO 设备 DMA", "通过；合法 fault 可恢复，错误设备身份和越界查询被拒绝。"),
        ("多 VFIO 设备", "通过；不同 RID 和 IOMMU group 建立独立设备关系，并分别完成回收。"),
        ("MSI/MRIF", "通过；正向目标可用，错误 device/vCPU/IID 和 unbound 目标被拒绝，支持 retarget。"),
        ("PRI", "通过；合法页面请求可恢复，非法、停止和错误场景返回拒绝或错误响应。"),
        ("STOP/DMA", "通过；在途 DMA、STOP 后新 DMA 和授权查询均受到阻断。"),
        ("生命周期与资源回滚", "通过；覆盖并发、强制退出、资源耗尽、根页表和 IOMMU 映射回滚。"),
    ], [2200, 6600])
    b += heading(1, "5. 结论边界")
    b += para("本报告的“通过”表示原型系统已经具备并验证了机密虚拟机与典型 PCIe/VFIO 设备之间的受控 I/O 通路。它不等同于完整可信执行环境安全认证，也不证明所有设备类型、所有并发时序、零泄漏、恶意 Host 隔离或真实设备认证均已完成。")
    b += bullet("未实现或未由本轮证据证明：真实 SPDM/TDISP/IDE 认证和链路保护。")
    b += bullet("Guest IRQ handler 的端到端中断计数验证仍需补充。")
    b += bullet("真实设备上的 DMA、MSI、PRI 与 STOP 并发矩阵仍需独立验证。")
    b += bullet("长期创建/销毁压力下的内存、引用计数和 IOMMU 映射统计仍需补充。")
    b += heading(1, "6. 证据来源")
    b += para("主要证据来自项目内的安全边界说明及绑定、解绑、中断验证报告，覆盖 COVG、VFIO lazy fault、多设备、MSI/MRIF、PRI、STOP/DMA、生命周期和回滚测试。相关脚本输出要求同时存在 PASS 标记、Guest 完成和每个 TDI 的 teardown 记录，并拒绝 FAIL、panic、warning 和未撤销授权标记。")
    return b

def detailed_body():
    b = common_cover("安全 I/O 通路原型详细验证报告", "机密虚拟机与典型 I/O 设备通路保护的实现、证据与边界")
    b += heading(1, "1. 执行摘要")
    b += para("本报告根据 xs-cvm 当前工作区的实现、已有验证记录和自动化测试判据，对“验证虚拟机可信执行环境安全 I/O 通路原型验证系统支持机密虚拟机与典型 I/O 设备的通路保护”进行详细评估。评估结果为：原型验证范围内通过。系统已覆盖 CVM 创建和运行、VFIO PCI 设备绑定、Guest 接受、MMIO/DMA/IRQ 授权、MSI/MRIF、PRI、STOP、解绑以及异常回滚。")
    b += para("结论的关键限定是：系统证明了软件原型中的归属、范围、状态和撤销机制能够工作；不能将这些结果直接解释为真实设备认证、链路保密或抵抗任意恶意 Host 的完整安全证明。")
    b += heading(1, "2. 验证对象和要求")
    b += table(["项目", "内容"], [
        ("被测系统", "xs-cvm RISC-V 机密虚拟机与 CoVE-IO 安全 I/O 原型。"),
        ("核心组件", "OpenSBI CVM Monitor、Linux/KVM、VFIO、RISC-V IOMMU、kvmtool、Guest COVG。"),
        ("典型设备", "VFIO PCI 设备，覆盖单设备、双设备、MSI/MSI-X 和 PRI 场景。"),
        ("目标要求", "支持机密虚拟机与典型 I/O 设备之间的安全 I/O 通路保护。"),
        ("判定性质", "原型机制和可观测验证结果，不是产品认证或形式化证明。"),
    ], [2300, 6500])
    b += heading(1, "3. 保护目标和判定准则")
    b += para("本次将“通路保护”拆成六项可观察性质。每项性质必须有实现路径、正向证据和负向判据，才能计入通过结论。")
    b += table(["编号", "保护性质", "通过判据"], [
        ("P1", "设备归属", "设备只能关联指定 CVM；外来 CVM 或错误 owner 查询失败。"),
        ("P2", "设备身份", "PCI RID/device_id 与绑定关系一致；错误身份和未知类型被拒绝。"),
        ("P3", "访问范围", "MMIO、DMA 和 IRQ 请求必须落在注册范围内。"),
        ("P4", "运行状态", "只有 Guest 接受后的 STARTED TDI 才能命中运行态授权。"),
        ("P5", "撤销生效", "STOP/解绑后旧授权、映射、IRQ 目标和查询不能继续生效。"),
        ("P6", "失败回滚", "中途分配、映射或绑定失败后不能残留可用授权，后续重试可进行。"),
    ], [900, 2600, 5300])
    b += heading(1, "4. 系统架构和信任边界")
    b += para("安全 I/O 通路由四个功能区域组成。Guest 负责设备接受和驱动运行；Host KVM/VFIO 负责设备生命周期和映射配置；CVM Monitor 维护跨层一致的 TDI 状态；IOMMU 负责 DMA 地址翻译和中断重映射。")
    b += image_paragraph() + para("图1  安全 I/O 通路架构。虚线为管理授权，实线分别表示 MMIO、DMA 和中断通路。", "Caption", "center")
    b += heading(2, "4.1 TDI 生命周期")
    b += para("TDI 是 CVM 与 I/O 设备之间的状态化安全接口。Host 先注册接口、配置 MMIO/DMA/IRQ 并绑定 CVM；Guest 读取接口报告并完成 COVG 接受；接口进入 STARTED 后，Host 侧运行时查询才能获得授权。停止时，TDI 进入 STOPPING，撤销运行态访问后再执行资源清理和注销。")
    b += table(["状态", "含义", "可用操作"], [
        ("FREE", "接口未注册或已完全回收", "重新注册"),
        ("REGISTERED", "已存在但未绑定 CVM", "配置 MMIO、绑定"),
        ("BOUND", "已绑定 CVM，等待 Guest 接受", "Guest 检查并启动"),
        ("STARTED", "Guest 已接受，允许运行态查询", "MMIO、DMA、IRQ 运行访问"),
        ("STOPPING/ERROR", "运行态授权正在撤销或出现错误", "清理、回滚、最终停止"),
    ], [1800, 3900, 3100])
    b += heading(2, "4.2 Host 与 Guest 的职责分离")
    b += bullet("Host：发现设备、建立 VFIO 绑定、提交 TDI 配置、发起停止和回收。")
    b += bullet("Guest：枚举 owner-scoped TDI、核验报告、确认 MMIO 范围并调用 COVG start。")
    b += bullet("Monitor：判断 owner、状态、generation、范围和接口操作是否合法。")
    b += bullet("IOMMU：执行设备到内存的地址约束，并维护 MSI/MRIF 重映射状态。")
    b += heading(1, "5. 各类 I/O 通路验证")
    b += heading(2, "5.1 设备绑定和身份校验")
    b += para("绑定路径使用 VFIO PCI 设备的 requester ID、IOMMU group、MMIO BAR 和 restricted DMA window 创建 TDI。Linux/KVM 在 DMA_MAP 路径校验 PCI requester ID、IOMMU group 和 IOMMU fwspec；OpenSBI 侧进一步要求 owner 匹配、设备类型有效和 TDI 状态正确。")
    b += para("已有 vfio-lazy、vfio-multi 和 vfio-msi 前置 probe 验证了合法设备允许、错误设备身份拒绝、错误 RID 拒绝以及未知设备类型拒绝。多设备测试使用不同 RID 和不同 IOMMU group，证明设备身份不是只依据 IOVA 判断。")
    b += heading(2, "5.2 Guest 接受和 COVG 状态验证")
    b += para("Guest COVG 流程执行 owner-scoped 枚举、链路和报告查询、generation 检查、MMIO map 检查、start/stop 状态检查以及重复调用的错误判定。测试覆盖空接口、错误 GPA、错误大小、过期 generation、重复 map、重复 start、停止后重启等路径。")
    b += para("COVG 自测要求 Guest 完成并打印 PASS；Host 脚本还要求每个绑定 TDI 具有 teardown verified 记录。因此，仅有进程正常退出或 Host autorun PASS 不能单独判定通过。")
    b += heading(2, "5.3 MMIO 通路")
    b += para("MMIO 资源在 TDI 中记录 GPA 和长度，Guest 只能对报告中的精确范围执行 map 检查。运行时查询必须命中 owner 匹配且处于 STARTED 的接口。STOP 会清除 MMIO map generation，解绑路径进一步回收 MMIO window。")
    b += table(["正向检查", "负向检查"], [
        ("报告中的 GPA 和大小可完成 map；map generation 与接口 generation 一致。", "错误 GPA、错误大小、空接口和过期 generation 被拒绝。"),
        ("STARTED TDI 可被运行态查询命中。", "BOUND、STOPPING 或非 owner 查询不能获得运行态授权。"),
    ], [4800, 4800])
    b += heading(2, "5.4 DMA 通路")
    b += para("DMA 验证覆盖受限 DMA window、lazy fault recovery、私有页直接 DMA、PCI RID 约束和 IOMMU 映射回滚。合法请求首次可能产生 translation fault，随后由授权路径恢复映射；错误设备身份或越界 IOVA 不应恢复映射。")
    b += table(["场景", "证据和判定"], [
        ("单设备 lazy fault", "首次 fault，授权恢复后转换成功，相邻页也可恢复；通过。"),
        ("多设备", "不同 RID/group 的两个设备分别建立和回收映射；通过。"),
        ("错误身份", "wrong-device、wrong-rid、unknown-type probe 被拒绝；通过。"),
        ("STOP/DMA", "在途 DMA、STOP 后新 DMA 和授权查询均被阻断；通过。"),
        ("IOMMU -ENOMEM", "失败映射不保留 IOTLB 条目，同一 IOVA 可重试；通过。"),
    ], [2500, 7100])
    b += heading(2, "5.5 MSI、MSI-X 和 MRIF 中断通路")
    b += para("中断绑定记录 GSI、PCI-RID device_id、目标 vCPU 和 IMSIC IID。Linux/KVM 在注入前查询 OpenSBI 的 FIND_IRQ 授权；RISC-V IOMMU 路径安装 per-device MSI/MRIF 映射。")
    b += bullet("正向目标、正确设备和正确 IID 可以完成授权。")
    b += bullet("错误 device、错误 vCPU、错误 IID 和 unbound 目标被拒绝。")
    b += bullet("retarget 时旧目标撤销，新目标授权；VS-file 未就绪时保持 MSI blocked。")
    b += bullet("IRQ unbind 和 STOP 后旧中断目标不能继续命中。")
    b += heading(2, "5.6 PRI 页面请求通路")
    b += para("PRI 验证覆盖合法页面请求、非法 IOVA、停止路径、请求取消、队列溢出和 PASID-valid 扩展模式。COVE-IO 检查设备归属和地址授权后，才允许 fault recovery 并返回成功 PRGR；停止或非法请求返回拒绝/错误响应。")
    b += heading(2, "5.7 STOP、解绑和销毁路径")
    b += para("显式 teardown 和 VM destroy fallback 都会执行对称清理。清理顺序包括 STOP、IRQ_UNBIND、DMA_UNMAP、RECLAIM_MMIO、UNBIND、generation 更新以及 TDI 注销。OpenSBI 的 FIND_MMIO/FIND_DMA/FIND_IRQ 只查找 STARTED 且 owner 匹配的接口，因此停止后旧查询不能获得运行态授权。")
    b += heading(1, "6. 验证矩阵")
    b += table(["编号", "验证项目", "验证方式", "结果"], [
        ("V-01", "CVM 创建和 COVG 接受", "COVG 自测、Guest PASS、TDI teardown", "通过"),
        ("V-02", "单设备绑定和 DMA", "vfio-lazy：fault、恢复、相邻页、teardown", "通过"),
        ("V-03", "多设备身份隔离", "vfio-multi：不同 RID/group、独立 IOAS/HWPT", "通过"),
        ("V-04", "错误设备身份", "wrong-device、wrong-rid、unknown-type probe", "通过"),
        ("V-05", "MSI/MRIF 基础路径", "正向目标、负向目标、Guest request/ack", "通过"),
        ("V-06", "MRIF retarget", "多 vCPU retarget stress、旧目标撤销", "通过"),
        ("V-07", "PRI", "basic、stop、cancel、overflow、PASID/extended", "通过"),
        ("V-08", "STOP 与 DMA", "在途 DMA、STOP 后 DMA、授权查询", "通过"),
        ("V-09", "生命周期", "顺序、并发、强制退出恢复、generation", "通过"),
        ("V-10", "资源耗尽", "CVM/TDI/IRQ 容量、回收和重试", "通过"),
        ("V-11", "失败回滚", "根页表 ownership、IOMMU -ENOMEM、MRIF 绑定", "通过"),
    ], [900, 2600, 5000, 900])
    b += heading(1, "7. 实现证据映射")
    b += table(["功能", "主要实现位置", "验证关注点"], [
        ("CVM 与 TDI 状态机", "opensbi/lib/sbi/sbi_cvm.c", "owner、状态、generation、MMIO/DMA/IRQ 查询"),
        ("Host TDI 操作", "kvmtool/cove-io.c", "注册、绑定、探测、停止、清理和 stale generation"),
        ("CVM 设备身份和 DMA", "linux/arch/riscv/kvm/vcpu.c", "PCI RID、IOMMU group、设备归属和撤销"),
        ("IOMMU 通路", "linux/drivers/iommu/riscv/iommu.c", "lazy fault、STOP、MSI/MRIF、PRI、回滚"),
        ("Guest 接受", "linux/arch/riscv/kernel/cove_io_guest.c", "枚举、报告、MMIO map、start/stop 自测"),
        ("自动化判据", "scripts/cove-io-*.sh", "PASS、FAIL、teardown、超时和负向结果"),
    ], [2500, 4000, 2900])
    b += heading(1, "8. 证据质量和重复性")
    b += para("验证脚本不是只检查退出码，而是同时检查串口中的 PASS 标志、Guest 完成、TDI 绑定和逐 TDI teardown；出现 FAIL、panic、BUG、WARNING、未撤销授权或 stale generation 被接受时判定失败。外层运行入口还检查超时和进程退出状态。")
    b += para("项目提供独立 Host/Guest 构建目录、统一 staging 打包、源码状态和 SHA-256 清单。清单能够说明打包时的输入快照，但不能替代从空目录完整重建的可信溯源证明；因此报告将已有镜像验证和当前源码对应关系分开表述。")
    b += heading(1, "9. 局限、残余风险和未覆盖项")
    b += table(["类别", "当前边界"], [
        ("设备认证", "COVG 报告明确为模拟语义；真实 transcript、SPDM、设备 measurement 和 whitelist 尚未实现。"),
        ("链路安全", "未证明 PCIe 链路机密性、完整性及 IDE 等硬件保护。"),
        ("恶意 Host", "当前证据主要证明 owner、身份、范围、generation 和软件撤销，不等同于恶意 Host 威胁模型证明。"),
        ("中断端到端", "已有 Guest request/ack 和 remap 证据，但尚未由 Guest IRQ handler 递增计数证明最终投递。"),
        ("真实设备并发", "STOP 与已提交 DMA/MSI/PRI 的真实设备并发矩阵仍需验证。"),
        ("长期稳定性", "尚未完成长时间创建/销毁下的内存、引用计数和 IOMMU 映射数量统计。"),
        ("VFIO 覆盖面", "其它 VFIO backend、全部硬件故障组合和真实硬件尚未完成全矩阵复测。"),
    ], [2300, 7100])
    b += heading(1, "10. 后续验证建议")
    for item in [
        "在 Guest 内安装真实 IRQ handler，以计数、目标 vCPU 和负向 IID/RID 验证中断端到端投递。",
        "使用真实设备验证 STOP 与已提交 DMA、MSI 和 PRI 请求的竞态行为。",
        "构造两个同时运行的 CVM，执行相互访问对方 TDI、MMIO、DMA 和 IRQ 目标的负向测试。",
        "执行长时间创建/销毁压力测试，记录内存、引用计数、TDI、IRQ range 和 IOMMU 映射数量。",
        "接入 SPDM/TDISP/IDE 和设备 measurement 校验，并单独定义、评审和审计威胁模型。",
    ]:
        b += number(item)
    b += heading(1, "11. 最终判定")
    b += rich_para([("判定：", True, False, "1F4E78", 24), ("通过（原型验证范围）。", True, False, "287B55", 24)])
    b += para("现有系统已经支持机密虚拟机与典型 PCIe/VFIO I/O 设备之间的安全通路建立、运行态访问控制、设备身份约束、中断和 DMA 隔离、停止撤销以及失败回滚。该结论适用于原型验证目标，不应替代生产级可信执行环境认证或真实硬件安全评估。")
    b += heading(1, "附录 A：已有验证记录")
    b += bullet("docs/cove-io-bind-verification-report.md：设备绑定、设备身份、DMA 和多设备验证。")
    b += bullet("docs/cove-io-unbind-verification-report.md：停止、解绑、DMA 映射撤销和销毁清理验证。")
    b += bullet("docs/cove-io-interrupt-verification-report.md：MSI/MRIF、retarget 和负向中断目标验证。")
    b += bullet("docs/security-and-validation.md：安全边界、证据分层和未覆盖项。")
    b += bullet("scripts/cove-io-*.sh：运行时自动化验证入口和 PASS/FAIL 判据。")
    return b

def main():
    package_docx(OUT / "安全IO通路原型验证报告.docx", "安全 I/O 通路原型验证报告", short_body())
    package_docx(OUT / "安全IO通路原型详细验证报告.docx", "安全 I/O 通路原型详细验证报告", detailed_body())
    print(OUT / "安全IO通路原型验证报告.docx")
    print(OUT / "安全IO通路原型详细验证报告.docx")

if __name__ == "__main__":
    main()
