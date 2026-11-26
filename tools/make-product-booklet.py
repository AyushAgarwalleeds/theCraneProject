"""Build the customer booklet, A4 print spreads, and matching web schematics."""
from copy import deepcopy
from pathlib import Path
import shutil
import subprocess

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4, A5, landscape
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen.canvas import Canvas
from reportlab.platypus import Paragraph, Table, TableStyle
from pypdf import PdfReader, PdfWriter, Transformation

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "downloads"
ASSETS = ROOT / "docs" / "guide-assets"
TEMP = ROOT / "tmp" / "pdfs" / "product-booklet"
for directory in (OUTPUT, ASSETS, TEMP):
    directory.mkdir(parents=True, exist_ok=True)

pdfmetrics.registerFont(TTFont("Guide", "C:/Windows/Fonts/segoeui.ttf"))
pdfmetrics.registerFont(TTFont("Guide-Bold", "C:/Windows/Fonts/segoeuib.ttf"))
pdfmetrics.registerFontFamily("Guide", normal="Guide", bold="Guide-Bold", italic="Guide", boldItalic="Guide-Bold")
INK = colors.HexColor("#142A32")
MUTED = colors.HexColor("#48606A")
GREEN = colors.HexColor("#00665B")
RED = colors.HexColor("#B13B29")
LINE = colors.HexColor("#C8D8DD")
PALE = colors.HexColor("#ECF5F2")
WHITE = colors.white


class Schematic:
    def __init__(self, canvas, height):
        self.c = canvas
        self.h = height

    def text(self, x, y, label, size=14, bold=False, color=INK, align="left"):
        self.c.setFont("Guide-Bold" if bold else "Guide", size)
        self.c.setFillColor(color)
        method = {"left": self.c.drawString, "center": self.c.drawCentredString, "right": self.c.drawRightString}[align]
        method(x, self.h - y, label)

    def line(self, points, color=INK, weight=2):
        self.c.setStrokeColor(color)
        self.c.setLineWidth(weight)
        self.c.setLineCap(1)
        path = self.c.beginPath()
        path.moveTo(points[0][0], self.h - points[0][1])
        for x, y in points[1:]:
            path.lineTo(x, self.h - y)
        self.c.drawPath(path)

    def box(self, x, y, width, height, label=None, fill=WHITE):
        self.c.setFillColor(fill)
        self.c.setStrokeColor(LINE)
        self.c.setLineWidth(1)
        self.c.roundRect(x, self.h - y - height, width, height, 10, fill=1, stroke=1)
        if label:
            self.text(x + width / 2, y + 28, label, bold=True, align="center")

    def dot(self, x, y, color=GREEN):
        self.c.setFillColor(color)
        self.c.circle(x, self.h - y, 4, fill=1, stroke=0)

    def resistor(self, x, y1, y2, color=INK):
        step = (y2 - y1) / 8
        points = [(x, y1), (x, y1 + step)]
        points += [(x + (6 if i % 2 else -6), y1 + step * i) for i in range(2, 7)]
        points += [(x, y2 - step), (x, y2)]
        self.line(points, color)


def sender_diagram(d):
    d.box(30, 62, 190, 203, "ESP32 SENDER")
    d.text(195, 148, "GPIO4", bold=True, align="right", color=GREEN)
    d.text(195, 228, "GND", bold=True, align="right")
    d.line([(220, 144), (374, 144)], GREEN)
    d.line([(416, 144), (565, 144), (565, 224), (220, 224)])
    d.dot(374, 144)
    d.dot(416, 144)
    d.line([(374, 144), (409, 123)], GREEN)
    d.text(398, 91, "UP BUTTON", bold=True, align="center")
    d.text(398, 176, "Closes when pressed", 13, align="center")
    d.text(360, 296, "Repeat this GPIO-to-GND connection for each sender button.", 15, align="center")


def receiver_diagram(d):
    d.box(25, 25, 195, 264, "ESP32 RECEIVER")
    d.box(443, 25, 246, 264, "RELAY MODULE")
    for index, pin in enumerate([26, 21, 33, 32, 27, 14, 13, 4]):
        y = 80 + index * 23
        d.text(201, y + 4, f"GPIO{pin}", 13, align="right")
        d.line([(220, y), (443, y)], GREEN, 1.5)
        d.text(460, y + 4, f"IN{index + 1}", 13, bold=True)
        d.text(668, y + 4, ["Up", "Down", "Left", "Right", "Front", "Back", "Power", "Enable"][index], 12, align="right")
    d.text(331, 39, "3.3 V-compatible", 12, align="center", color=GREEN)
    d.text(331, 55, "active-low inputs", 12, align="center", color=GREEN)
    d.text(360, 312, "External module supply and common input GND as specified by your module.", 12, align="center")


def battery_diagram(d):
    d.box(24, 67, 156, 264, "PROTECTED OUT")
    d.text(163, 113, "OUT+", 16, bold=True, align="right", color=RED)
    d.text(163, 307, "OUT-", 16, bold=True, align="right")
    d.box(545, 178, 151, 153, "ESP32 SENDER")
    d.text(560, 244, "GPIO34", 17, bold=True, color=GREEN)
    d.text(560, 306, "GND", 16, bold=True)
    d.line([(180, 108), (215, 108)], RED)
    d.dot(215, 108, RED)
    d.dot(255, 108, RED)
    d.line([(215, 108), (249, 90)], RED)
    d.text(237, 145, "Optional switch", 12, align="center")
    d.line([(255, 108), (340, 108), (340, 158)], RED)
    d.line([(340, 108), (340, 44), (620, 44)], RED)
    d.text(480, 32, "To boost IN+ (pack voltage)", 13, align="center", color=RED)
    d.resistor(340, 158, 202, RED)
    d.text(321, 186, "10 kΩ", 17, bold=True, align="right")
    d.line([(340, 202), (340, 241), (545, 241)], GREEN)
    d.dot(340, 241)
    d.text(323, 246, "Junction", 13, bold=True, align="right", color=GREEN)
    d.line([(340, 241), (340, 259)], GREEN)
    d.resistor(340, 259, 287)
    d.text(321, 283, "10 kΩ", 17, bold=True, align="right")
    d.line([(340, 287), (340, 302)])
    d.line([(180, 302), (545, 302)])
    d.dot(340, 302, INK)
    d.line([(444, 241), (444, 263)], GREEN, 1.5)
    d.line([(435, 263), (453, 263)], GREEN, 1.5)
    d.line([(435, 269), (453, 269)], INK, 1.5)
    d.line([(444, 269), (444, 302)], INK, 1.5)
    d.dot(444, 241)
    d.dot(444, 302, INK)
    d.text(460, 271, "100 nF", 12, bold=True)
    d.text(460, 286, "optional", 11)
    d.text(380, 358, "Red probe at junction. Black probe at OUT-.", 16, bold=True, align="center")
    d.text(380, 383, "A 4.20 V pack should give about 2.10 V at the junction.", 14, align="center", color=GREEN)


def power_diagram(d):
    for x, w, title in [(20, 133, "MATCHED 1S PACK"), (195, 149, "CHARGER"), (392, 134, "1S BOOST"), (568, 132, "ESP32 BOARD")]:
        d.box(x, 62, w, 161, title)
    d.text(86, 123, "+ parallel +", 13, align="center")
    d.text(86, 152, "- parallel -", 13, align="center")
    d.text(86, 193, "Up to 4.2 V", 13, bold=True, align="center")
    d.text(207, 118, "B+", 12)
    d.text(207, 164, "B-", 12)
    d.text(331, 118, "OUT+", 12, align="right")
    d.text(331, 164, "OUT-", 12, align="right")
    d.text(459, 191, "Set to 5.0 V", 13, bold=True, align="center", color=GREEN)
    d.text(633, 118, "5V input", 13, align="center")
    d.text(633, 164, "GND", 13, align="center")
    for a, b in [(153, 195), (344, 392), (526, 568)]:
        d.line([(a, 113), (b, 113)], RED)
        d.line([(a, 159), (b, 159)])
    d.line([(363, 113), (363, 24)], GREEN)
    d.dot(363, 113, GREEN)
    d.text(363, 16, "Optional switch and divider go on this pack-voltage feed", 13, align="center", color=GREEN)
    d.text(360, 260, "Disconnect sender power for charging. Use one ESP32 supply at a time.", 13, align="center")
    d.text(360, 282, "Meter the boost output before connecting the board.", 13, bold=True, align="center")


DIAGRAMS = {"sender": (720, 320, sender_diagram), "receiver": (720, 320, receiver_diagram),
            "battery": (720, 400, battery_diagram), "power": (720, 300, power_diagram)}
for name, (width, height, draw) in DIAGRAMS.items():
    target = TEMP / f"{name}.pdf"
    diagram_canvas = Canvas(str(target), pagesize=(width, height))
    diagram_canvas.setFillColor(colors.HexColor("#F5F9FA"))
    diagram_canvas.rect(0, 0, width, height, fill=1, stroke=0)
    draw(Schematic(diagram_canvas, height))
    diagram_canvas.save()
    subprocess.run([shutil.which("pdftoppm"), "-singlefile", "-r", "144", "-png", str(target), str(ASSETS / name)], check=True)


class Booklet:
    def __init__(self, path):
        self.c = Canvas(str(path), pagesize=A5)
        self.c.setTitle("CraneLink | Setup and connection handbook")
        self.c.setAuthor("CraneLink")
        self.width, self.height = A5
        self.margin = 31
        self.body_width = self.width - 2 * self.margin
        self.page = 0
        self.y = 0
        self.body = ParagraphStyle("Body", fontName="Guide", fontSize=10.4, leading=14.4, textColor=INK, spaceAfter=8)
        self.small = ParagraphStyle("Small", parent=self.body, fontSize=8.5, leading=11.5)

    def text(self, x, y, value, size=10, color=INK, bold=False):
        self.c.setFont("Guide-Bold" if bold else "Guide", size)
        self.c.setFillColor(color)
        self.c.drawString(x, self.height - y, value)

    def start(self, title, lead=None):
        if self.page:
            self.finish()
        self.page += 1
        self.y = 38
        self.text(self.margin, self.y, "CRANELINK  /  HANDBOOK", 8, GREEN, True)
        self.y += 29
        heading = Paragraph(title, ParagraphStyle("Title", fontName="Guide-Bold", fontSize=24, leading=26.8, textColor=INK))
        _, height = heading.wrap(self.body_width, 200)
        heading.drawOn(self.c, self.margin, self.height - self.y - height)
        self.y += height + 13
        if lead:
            self.p(lead)

    def p(self, value, small=False):
        paragraph = Paragraph(value, self.small if small else self.body)
        _, height = paragraph.wrap(self.body_width, 500)
        paragraph.drawOn(self.c, self.margin, self.height - self.y - height)
        self.y += height + 8

    def h(self, value):
        self.y += 4
        self.text(self.margin, self.y + 12, value, 12, GREEN, True)
        self.y += 24

    def steps(self, values):
        for number, value in enumerate(values, 1):
            self.p(f"<b>{number}.</b> {value}")

    def note(self, value):
        paragraph = Paragraph(value, self.small)
        _, height = paragraph.wrap(self.body_width - 18, 500)
        self.c.setFillColor(PALE)
        self.c.roundRect(self.margin, self.height - self.y - height - 16, self.body_width, height + 16, 5, fill=1, stroke=0)
        paragraph.drawOn(self.c, self.margin + 9, self.height - self.y - height - 8)
        self.y += height + 27

    def table(self, headers, rows, widths=None):
        values = [[Paragraph(str(cell), self.small) for cell in row] for row in [headers, *rows]]
        widths = widths or [self.body_width / len(headers)] * len(headers)
        table = Table(values, colWidths=widths, hAlign="LEFT")
        table.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, 0), PALE), ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("LINEBELOW", (0, 0), (-1, -1), .5, LINE),
            ("LEFTPADDING", (0, 0), (-1, -1), 7), ("RIGHTPADDING", (0, 0), (-1, -1), 7),
            ("TOPPADDING", (0, 0), (-1, -1), 6), ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
        ]))
        _, height = table.wrap(self.body_width, 500)
        table.drawOn(self.c, self.margin, self.height - self.y - height)
        self.y += height + 12

    def diagram(self, name):
        width, height, draw = DIAGRAMS[name]
        scale = self.body_width / width
        self.c.saveState()
        self.c.translate(self.margin, self.height - self.y - height * scale)
        self.c.scale(scale, scale)
        draw(Schematic(self.c, height))
        self.c.restoreState()
        self.y += height * scale + 10

    def record(self, label):
        self.text(self.margin, self.y + 10, label, 9, MUTED)
        self.c.setStrokeColor(LINE)
        self.c.line(self.margin, self.height - self.y - 27, self.width - self.margin, self.height - self.y - 27)
        self.y += 41

    def finish(self):
        if self.y > self.height - 40:
            raise RuntimeError(f"Content crosses footer on page {self.page}: {self.y:.1f}")
        self.c.setStrokeColor(LINE)
        self.c.line(self.margin, 30, self.width - self.margin, 30)
        self.text(self.margin, self.height - 18, "CRANELINK  ·  EDITION 3", 7.5, MUTED)
        self.text(self.width - self.margin - 14, self.height - 18, str(self.page), 8, MUTED, True)
        self.c.showPage()

    def save(self):
        self.finish()
        if self.page != 16:
            raise RuntimeError("The booklet must contain 16 pages for four-sheet printing.")
        self.c.save()


path = OUTPUT / "CraneLink-Product-Booklet.pdf"
b = Booklet(path)
b.start("Your CraneLink<br/>handbook.", "Setup, connections and everyday use<br/>ESP32-WROOM relay bench edition")
b.y += 18
b.diagram("sender")
b.y += 8
b.p("<b>Find your next step</b>")
b.p("First setup 3 · Controls 4 · Wireless pairing 6-7<br/>Sender wiring 8 · Receiver wiring 9 · Battery 10-12<br/>Settings 13 · Troubleshooting 14 · Equipment record 16", small=True)
b.note("Keep relay contacts disconnected from machinery and 110 V wiring during setup and testing. Crane service requires an independent safety-rated stop circuit and a qualified controls review.")
b.p("Online help and installer<br/><b>thecraneproject.vercel.app</b><br/>Guide edition 3 · 26 November 2026", small=True)

b.start("Before you connect.")
b.table(["Have ready", "What it is for"], [
    ["Classic ESP32-WROOM boards", "Receiver plus one or more handhelds. At least 4 MB flash per board."],
    ["USB data cable", "First upload and USB diagnostics. Android may need an OTG adapter."],
    ["Momentary switches", "Each handheld button closes a GPIO to the handheld's GND."],
    ["Compatible relay module", "Eight active-low inputs compatible with 3.3 V logic, plus its specified external supply."],
    ["Meter and small parts", "Continuity/voltage checks, divider resistors, LED resistor and a suitable piezo."],
], [128, b.body_width - 128])
b.h("Know the two pages")
b.p("<b>Online installer</b> runs in your browser over the internet. Use desktop Chrome or Edge, or Android Chrome with a supported USB host connection.")
b.p("<b>Device page</b> opens at <b>http://192.168.4.1/</b> while joined to the receiver's Wi-Fi or the handheld's setup Wi-Fi. Its title tells you which board you reached.")
b.note("The software Emergency Stop commands outputs off. Relay contact feedback and safety certification are outside this bench build. Check the real contacts during bench tests.")
b.p("Sources used for connection guidance<br/><link href='https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html' color='#00665B'>Espressif DevKitC power inputs</link> · <link href='https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/schematic-checklist.html#adc' color='#00665B'>Espressif ADC guidance</link>", small=True)

b.start("First USB setup.", "Use the online installer with the receiver and all its handhelds ready.")
b.steps([
    "Open <b>thecraneproject.vercel.app</b>. Under <b>Install and pair</b>, choose one to four handhelds and press <b>Start setup</b>.",
    "Connect the receiver's USB port. Choose <b>Connect and upload</b>, pick its port, and wait for pairing to be saved.",
    "Unplug the receiver. Connect and upload each sender when prompted. Complete every sender in the same browser tab.",
    "Choose <b>Download PDF</b> to keep the private setup report. Power the boards. Reset the startup Stop (page 4), then test a fresh direction press.",
])
b.note("This workflow replaces the receiver's sender list. For adding a master to existing receivers, use the wireless process on pages 6-7.")
b.h("Save the private report")
b.p("Keep the station MACs, receiver Wi-Fi name and password, and handheld setup Wi-Fi credentials. Record relay settings separately on page 16.")
b.h("If the upload stops")
b.p("After a successful upload with failed pairing, reconnect the same board and use <b>Retry pairing only</b> in the same tab. Use a full upload if the installer reports older firmware.")
b.p("Keep BOOT released after uploading. Tap EN/RESET once if the board stays in upload mode. Close other apps or tabs using the USB port.")

b.start("Know your buttons.", "After every receiver restart, all outputs are commanded off. Reset Emergency Stop as shown below. Disconnect the phone from receiver Wi-Fi for handheld control.")
b.table(["Action", "What to do"], [
    ["Direction", "Hold to jog. Release to stop that direction. Its relay and R8 follow the hold."],
    ["Power On/Off", "Hold for 3 seconds and release. Repeat the hold to toggle it off."],
    ["Emergency Stop", "Press to turn all eight relay outputs off and latch the stop."],
    ["Reset Emergency Stop", "Release all buttons. Hold Power + Emergency Stop for 3 seconds, then release both."],
], [119, b.body_width - 119])
b.h("Reset leaves the outputs off")
b.p("After reset, start a fresh direction press. On the phone page, you can also use <b>Reset Emergency Stop</b> after releasing the handheld's Stop button.")
b.h("Power stays latched")
b.p("Power defaults to 60 minutes of inactivity. Accepted directions and Power holds count as activity. Emergency Stop, loss of the controlling link, or receiver reboot also clears Power.")
b.note("On a master handheld, commands and Emergency Stop address the selected receiver. Count the receiver-number flashes before testing a direction.")

b.start("Use your phone.", "Reset Emergency Stop after receiver startup. The phone takes over when it joins the receiver's Wi-Fi.")
b.steps([
    "Join the receiver Wi-Fi named in your private setup report. Choose to stay connected if the phone reports no internet.",
    "Open <b>http://192.168.4.1/</b>. Confirm the page says <b>CraneLink Phone Bench</b>.",
    "Tap <b>Web Run</b>, then hold a direction to jog. Lift your finger to release.",
    "Disconnect the phone from receiver Wi-Fi to return control to the handheld. Release held handheld buttons and press again.",
])
b.note("Web Run off leaves the phone joined to Wi-Fi. Disconnect that Wi-Fi connection to give handheld motion control back.")
b.h("When a link is lost")
b.p("Motion and Power drop when commands expire (nominally 650 ms). After 60 seconds out of range, the handheld searches periodically to save battery. Tap Power to search immediately, release all buttons, then give a fresh command.")
b.p("A linked handheld keeps its radio awake all day. Wi-Fi can take about ten seconds to detect a phone that left range. The command timeout still applies. A Power tap does not reset Emergency Stop.")
b.h("One source at a time")
b.p("A second handheld whose press is blocked must release and press again. Both the phone and a paired handheld can issue Stop.")

b.start("Add an installed<br/>receiver.", "Use the receiver's Wi-Fi settings where it is installed. The master handheld can store up to four receiver entries.")
b.h("Prepare the handheld once")
b.p("In the online installer's <b>Wireless setup</b> section, connect only the handheld by USB and choose <b>Update handheld firmware</b>. Keep its <b>Station MAC</b> and its private <b>Setup Wi-Fi</b> name and password from Setup details.")
b.h("On each receiver")
b.steps([
    "Join the receiver's Wi-Fi and open <b>http://192.168.4.1/</b>. Confirm you see the receiver page.",
    "Open <b>Settings and sender pairing</b>. Choose an empty sender slot, or the existing slot for this same handheld.",
    "Paste the <b>handheld Station MAC</b> into <b>Sender station MAC</b>. Turn Web Run and Power off and release all controls.",
    "Tap <b>Generate pairing key</b>. Keep the <b>Receiver station MAC</b> and the <b>Shared key</b> available before tapping <b>Save sender slot</b>.",
])
b.note("Each receiver needs its own 32-character hexadecimal key. Register the same key on both sides. Copy the key before saving because the settings page clears it.")
b.p("Update every installed receiver to release 8 before its handhelds. A release 8 handheld requires receiver acknowledgments for motion and Power. Continue on page 7.")

b.start("Save it on the<br/>handheld.")
b.steps([
    "Release all buttons. Hold <b>Emergency Stop alone for 8 seconds</b>, then release. This opens setup without a power switch and latches Stop on the selected receiver.",
    "Join its <b>CraneLink-TX-...</b> Wi-Fi using the private setup password. Open or reload <b>http://192.168.4.1/</b>. Confirm it says <b>Handheld setup</b>.",
    "Enter a label such as <b>MH14AB4292</b>, the <b>Receiver station MAC</b>, and the same <b>Shared key</b> from that receiver. Tap <b>Save receiver</b>.",
    "Repeat as needed. Tap <b>Finish and return to radio</b>. Disconnect from receiver Wi-Fi. Hold Power + Stop for 3 seconds, then release both to reset Stop.",
])
b.h("Switch to the next receiver")
b.p("Tap Power for less than 0.3 seconds. Release, then press again within 0.6 seconds and hold for 2 seconds. Release and count the 1-4 LED flashes. Wait for solid link light, then press a direction.")
b.note("Tap and release Power to leave setup, or wait 15 minutes without use. Radio resumes on the selected receiver. After reboot it selects receiver 1. Holding Power for 3 seconds at startup also opens setup.")
b.h("USB alternative")
b.p("Use <b>Master sender</b> in the online installer. Read the handheld, add receivers over USB in order, then reconnect the same handheld and save. The wizard replaces the handheld receiver list.", small=True)

b.start("Wire the sender.", "Use the GPIO labels printed on the board. Each normally open button connects its GPIO to sender GND when pressed.")
b.diagram("sender")
b.table(["Function", "Sender GPIO"], [
    ["Up / Down", "4 / 32"], ["Left / Right", "33 / 25"], ["Front / Back", "26 / 27"],
    ["Power On/Off", "13"], ["Emergency Stop", "14"], ["Status LED", "23"],
    ["Passive piezo", "18"], ["Battery sense junction", "34"],
], [b.body_width * .61, b.body_width * .39])

b.start("Wire the receiver.", "These are low-voltage relay input connections. Keep relay contacts disconnected from the crane and 110 V wiring for all bench tests.")
b.table(["Module input", "Receiver GPIO", "Default action"], [
    ["IN1", "26", "Up"], ["IN2", "21", "Down"], ["IN3", "33", "Left"], ["IN4", "32", "Right"],
    ["IN5", "27", "Front"], ["IN6", "14", "Back"], ["IN7", "13", "Power latch"], ["IN8", "4", "Direction enable"],
], [108, 114, b.body_width - 222])
b.p("Use active-low, 3.3 V-compatible inputs. Supply the relay module externally at its rated voltage. For common-ground inputs, join module input GND and receiver GND. Confirm VCC/JD-VCC requirements from your module.", small=True)
b.note("R8 follows directional jogs. R1-R7 can be reassigned. Receiver IN2 uses GPIO21 in this release.")

b.start("Battery power path.", "Use a matched 1S parallel lithium-ion pack with suitable protection and a charger intended for 4.2 V charging.")
b.diagram("power")
b.steps([
    "Connect the pack to the charger's <b>B+</b> and <b>B-</b>. Use protected <b>OUT+</b> and <b>OUT-</b> for the load and sense circuit.",
    "A power switch is optional. Without one, join the switch points and use a disconnectable power lead. Feed the boost and divider from protected OUT+.",
    "Use a 1S-rated boost converter. Set and meter its output at <b>5.0 V</b> before connecting it to the development board's <b>5V input</b> and GND.",
    "Disconnect the sender load while charging. Disconnect boost 5V from the ESP32 before powering that board by USB. The Power button does not isolate its supply.",
])
b.note("A converter stamped XL6009 has a specified input starting at 5 V. Use a converter specified for the full 1S discharge range, including below the 3.50 V warning point.")
b.p("For a TP4056-type charger board, confirm B and OUT markings. The protected output follows pack voltage. <link href='https://www.toppwr.com/uploadfile/file/20240130/65b892bb04a3c.pdf' color='#00665B'>Charger datasheet</link> · <link href='https://www.mantech.co.za/Datasheets/Products/xl6009.pdf' color='#00665B'>XL6009 specifications</link>", small=True)

b.start("Battery divider.", "Two equal 10 kΩ resistors halve the voltage before it reaches sender GPIO34.")
b.diagram("battery")
b.steps([
    "Leave GPIO34 disconnected. With the pack on, measure the resistor junction against protected OUT-. Expect half the measured pack voltage.",
    "At a <b>4.20 V</b> pack, the junction should read about <b>2.10 V</b>. Turn the pack off, then connect the junction to GPIO34.",
    "An optional <b>100 nF</b> ceramic capacitor connects the junction to GND. Join protected OUT- to sender GND.",
    "With battery power off, plug in USB. Enable <b>Sender battery monitoring</b> in USB diagnostics. Unplug USB before battery power.",
])
b.note("Keep raw battery voltage off GPIO34. For a live-pack USB check, disconnect boost 5V from the ESP32. Keep the divider and ground connected.")

b.start("Light, sound<br/>and battery.")
b.table(["Part", "Connection"], [
    ["Status LED", "GPIO23 to 470 Ω resistor to LED anode. Cathode to GND. The flat side or shorter lead is normally the cathode."],
    ["Small passive piezo", "GPIO18 to piezo +; piezo - to GND. Use a part suitable for 3.3 V drive."],
], [105, b.body_width - 105])
b.table(["State", "LED / sound"], [
    ["Searching", "Short flash about once a second; periodic search tone. After a minute out of range: brief flash about every 3 seconds."],
    ["Commands acknowledged", "Solid light while the receiver acknowledges fresh packets. One beep on connection."],
    ["Link lost", "Rapid flash; two short beeps on loss."],
    ["Receiver selected", "One to four flashes identify the saved receiver number."],
    ["Setup Wi-Fi", "Repeating double flash while setup is active."],
    ["Battery low", "Rapid flash burst over the link pattern; three beeps every 30 seconds."],
], [105, b.body_width - 105])
b.p("The low warning starts near 3.50 V under load and clears above about 3.65 V. Compare readings with a meter. Battery voltage is an approximate condition indicator.")
b.note("A powered or magnetic buzzer needs a suitable driver and its rated supply. Direct GPIO18 drive is for a small passive piezo.")

b.start("Make it yours.")
b.h("Wi-Fi name and password")
b.p("Connect the receiver by USB and disconnect the phone from receiver Wi-Fi. In <b>Receiver Wi-Fi</b> on the installer, enter a 4-12 character alphanumeric name and an 8-63 character password without spaces. Choose <b>Apply over USB</b>. Names save in uppercase and the receiver restarts.")
b.h("Relay assignments")
b.p("On the receiver phone page, open <b>Settings and sender pairing</b>. Map the six directions and Power to R1-R7. Choosing an occupied relay swaps its previous assignment. Choose <b>Save relay settings</b>. R8 stays reserved for directional enable.")
b.h("Power inactivity timeout")
b.p("Choose 1-720 minutes. The default is 60. Directions and Power holds count as activity. Idle radio packets and an open phone page do not extend the timeout.")
b.h("Direction control")
b.p("<b>All directions together</b> is the bench default. <b>NO/NC interlock</b> accepts one direction per Up/Down, Left/Right and Front/Back pair. First held wins. A simultaneous opposing press keeps the pair off. Release both before reversing after a conflict.")
b.note("Turn Web Run and Power off and release all buttons before changing assignments or pairing. Software interlock controls direction commands. Use the module's markings for physical NO/NC contacts.")

b.start("Solve a problem.")
b.table(["What you see", "Check this"], [
    ["Handheld stops", "Disconnect phone Wi-Fi. Tap Power, release all buttons, then press a direction. Check selection and Stop latch. Read reset reason over USB; check stable 5 V."],
    ["Stop remains latched", "Release all buttons. Hold Power + Emergency Stop for 3 seconds, then release both. Start a new direction press."],
    ["Wi-Fi has no internet", "Stay connected and type http://192.168.4.1/. Reload when changing between receiver and handheld setup Wi-Fi."],
    ["USB board gives no reply", "Check the cable and selected port. Close other serial apps. Release BOOT, tap EN/RESET, then retry."],
    ["Button stays pressed", "Use Check connected board. Closed inputs should say All released. Inspect that GPIO-to-GND switch."],
    ["Relay LED only", "Check the external module supply and VCC/JD-VCC instructions. Meter the actual contacts on the disconnected bench."],
    ["No battery reading", "Enable monitoring. Check two equal 10 kΩ resistors, common GND and half-pack voltage at the junction."],
], [105, b.body_width - 105])
b.note("The phone's Commanded relays display reports output commands. Check the physical relay state independently with a continuity meter.")

b.start("Check and maintain.")
b.h("Before a bench session")
b.steps([
    "Inspect connections, power leads and battery condition. Confirm the receiver label and the handheld receiver-number flashes.",
    "Test each direction and release. Verify the matching relay and R8 energize and then release.",
    "Toggle Power with a 3-second hold. Test Emergency Stop while outputs are active. Meter that every output releases.",
    "Test receiver reboot and link loss. Measure contact release. Return with a button held and check a fresh press is needed. Repeat at the end of a 12-hour battery bench test.",
])
b.h("Replace a handheld")
b.p("For a blank handheld choose <b>Install on a new handheld</b>. Put its new Station MAC and a new key in the old handheld's slot on each receiver. Add those receiver MACs and keys through handheld Wi-Fi setup.")
b.h("Replace a receiver")
b.p("Use <b>Install and pair</b> and complete the handheld steps in that same session. Restore relay settings. Add the replacement's MAC and key to a master through wireless setup. Use the USB master wizard to rebuild a full four-entry receiver list.")
b.p("Keep the private setup report and the equipment record. Both are specific to your boards. The guide describes the current bench firmware and does not certify a machinery installation.", small=True)

b.start("Equipment record.", "Keep this page and the downloaded setup report private.")
b.record("Receiver label / location")
b.record("Receiver station MAC")
b.record("Handheld station MAC")
b.record("Receiver Wi-Fi name")
b.record("Power timeout / direction control")
b.table(["Master order", "Receiver label / station MAC"], [[str(i), " "] for i in range(1, 5)], [90, b.body_width - 90])
b.p("<b>Remember</b><br/>Power hold 3 s. Reset = Power + Stop hold 3 s, then release.<br/>Next receiver = tap Power, release, hold 2 s, release.<br/>Wake search = tap Power. Setup = Stop alone 8 s, then release.", small=True)
b.p("Help and downloads<br/><link href='https://thecraneproject.vercel.app/guide.html' color='#00665B'>thecraneproject.vercel.app/guide.html</link>", small=True)
b.save()

reader = PdfReader(path)
writer = PdfWriter()
writer.add_metadata({"/Title": "CraneLink | A4 fold-and-staple print booklet", "/Author": "CraneLink"})
sheet_width, sheet_height = landscape(A4)
order = []
for sheet in range(4):
    order.extend([(16 - 2 * sheet, 1 + 2 * sheet), (2 + 2 * sheet, 15 - 2 * sheet)])
for left, right in order:
    spread = writer.add_blank_page(width=sheet_width, height=sheet_height)
    for side, number in enumerate([left, right]):
        page = deepcopy(reader.pages[number - 1])
        page_width = float(page.mediabox.width)
        x = side * sheet_width / 2 + (sheet_width / 2 - page_width) / 2
        spread.merge_transformed_page(page, Transformation().translate(tx=x), over=True)
with (OUTPUT / "CraneLink-A4-Print-Booklet.pdf").open("wb") as handle:
    writer.write(handle)
assert len(reader.pages) == 16
assert len(PdfReader(OUTPUT / "CraneLink-A4-Print-Booklet.pdf").pages) == 8
local_output = ROOT / "output" / "pdf"
local_output.mkdir(parents=True, exist_ok=True)
for filename in ("CraneLink-Product-Booklet.pdf", "CraneLink-A4-Print-Booklet.pdf"):
    shutil.copyfile(OUTPUT / filename, local_output / filename)
print(f"Created {path}")
print(f"A4 spread order {order}")
