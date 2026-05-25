#!/usr/bin/env python3
import math
import os
import subprocess
import tkinter as tk
from collections import deque


ROUTES = ["Nord", "Sud", "Est", "Ouest"]
BG = "#050914"
CARD = "#0c1b2e"
CARD_2 = "#10243d"
ROAD = "#202a38"
ROAD_EDGE = "#405169"
ROAD_LINE = "#ecf3ff"
GREEN = "#25f28c"
RED = "#ff4d67"
YELLOW = "#ffd166"
CYAN = "#4de3ff"
MAGENTA = "#d66bff"
TEXT = "#f4fbff"
MUTED = "#91a8bd"
WAITING_CAR = "#8ca7bd"
STOP_LINE = "#ff5f73"
SHADOW = "#02050a"
STATE_FILE = "smartcross_live.txt"
EVENTS_FILE = "smartcross_events.txt"

ROUTE_EXPLAIN = {
    0: "North/South gets the green phase. Compatible opposite flow may pass safely.",
    1: "South/North gets the green phase. The controller keeps the conflict zone protected.",
    2: "East/West gets the green phase. Queued cars are released by the scheduler.",
    3: "West/East gets the green phase. Semaphores prevent unsafe overlap.",
}


def parse_state(path):
    data = {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                if "=" not in line:
                    continue
                key, value = line.strip().split("=", 1)
                data[key] = value
    except OSError:
        return {}
    return data


def as_int(data, key, default=0):
    try:
        return int(float(data.get(key, default)))
    except ValueError:
        return default


def as_float(data, key, default=0.0):
    try:
        return float(data.get(key, default))
    except ValueError:
        return default


class SmartCrossGame:
    """Game-style front-end for SmartCross.

    The Python process owns the user experience: menu, parameters, traffic view,
    and back-to-menu flow. When the user presses START, it launches the C engine
    with -no-gui. The C engine writes live state files that this GUI polls.
    """

    def __init__(self, root):
        self.root = root
        self.root.title("SmartCross - Traffic Control Game")
        self.root.configure(bg=BG)
        self.root.geometry("1360x860")
        self.root.minsize(1280, 820)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.lift()
        self.root.attributes("-topmost", True)
        self.root.after(1500, lambda: self.root.attributes("-topmost", False))

        self.canvas = tk.Canvas(root, bg=BG, highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Button-1>", self.on_click)

        self.mode = "menu"
        self.process = None
        self.state = {}
        self.events = deque(maxlen=10)
        self.last_event_size = 0
        self.frame = 0
        self.last_frame_ms = None
        self.particles = []
        self.last_passed = [0, 0, 0, 0]
        self.spawn_clock = [0, 0, 0, 0]
        self.display_values = {}
        self.buttons = []
        self.controls = {}
        self.config = {
            "mode": "priority",
            "strategy": "dynamic",
            "cars": 120,
            "processes": 5,
            "threads": 2,
            "speed": 1,
            "quantum": 2,
        }
        self.message = "Configure your simulation, then press START."
        self.tick()

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
        self.root.destroy()

    def tick(self):
        # Single animation loop for both screens. In simulation mode it also
        # polls the C engine state and advances smooth vehicle particles.
        self.frame += 1
        if self.mode == "running":
            latest = parse_state(STATE_FILE)
            if latest:
                self.state = latest
            self.read_events()
            self.update_particles()
            self.update_interpolated_values()
            if self.process and self.process.poll() is not None and as_int(self.state, "running", 1) == 0:
                self.mode = "finished"
                self.message = "Simulation complete. Review the results, then return to setup."
        self.draw()
        self.root.after(16, self.tick)

    def on_click(self, event):
        # Canvas buttons are simple rectangles registered in self.buttons.
        for button in list(self.buttons):
            x0, y0, x1, y1, action = button
            if x0 <= event.x <= x1 and y0 <= event.y <= y1:
                action()
                return

    def read_events(self):
        # Read only newly appended lines from the C event file.
        try:
            size = os.path.getsize(EVENTS_FILE)
        except OSError:
            return
        if size < self.last_event_size:
            self.last_event_size = 0
        try:
            with open(EVENTS_FILE, "r", encoding="utf-8") as handle:
                handle.seek(self.last_event_size)
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    self.events.appendleft(line.split("|", 1)[-1])
                self.last_event_size = handle.tell()
        except OSError:
            return

    def start_simulation(self):
        # Start the C simulation engine in the background. -no-gui is critical:
        # it prevents the C program from launching another Python GUI instance.
        if self.process and self.process.poll() is None:
            return
        for path in [STATE_FILE, EVENTS_FILE, "smartcross_live.tmp"]:
            try:
                os.remove(path)
            except OSError:
                pass
        self.state = {}
        self.events.clear()
        self.last_event_size = 0
        self.particles.clear()
        self.last_passed = [0, 0, 0, 0]
        self.spawn_clock = [0, 0, 0, 0]
        self.display_values.clear()
        self.message = "Simulation running. Watch the scheduler release traffic phases."
        command = [
            "./smartcross",
            "-mode", self.config["mode"],
            "-strategy", self.config["strategy"],
            "-cars", str(self.config["cars"]),
            "-p", str(self.config["processes"]),
            "-t", str(self.config["threads"]),
            "-quantum", str(self.config["quantum"]),
            "-speed", str(self.config["speed"]),
            "-no-gui",
        ]
        try:
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.mode = "running"
        except OSError as exc:
            self.message = f"Could not start ./smartcross: {exc}"

    def back_to_menu(self):
        self.mode = "menu"
        self.message = "Adjust parameters and launch another simulation."
        self.process = None

    def stop_simulation(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
        self.mode = "menu"
        self.message = "Simulation stopped. Configure another run."

    def update_interpolated_values(self):
        # The C engine updates state about every 50 ms. Interpolating counters
        # makes the control room feel smoother and easier to read.
        targets = {
            "passed": as_float(self.state, "total_passed", 0),
            "avg_wait": as_float(self.state, "avg_wait_ms", 0),
            "throughput": as_float(self.state, "throughput", 0),
            "collisions": as_float(self.state, "collisions_avoided", 0),
            "deadlocks": as_float(self.state, "deadlock_retries", 0),
        }
        for key, target in targets.items():
            current = self.display_values.get(key, target)
            self.display_values[key] = current + (target - current) * 0.12

    def update_particles(self):
        # Moving cars are visual particles independent from the exact C timing.
        # Passed-car deltas spawn real particles, and active green queues spawn
        # a few filler particles so the road does not look frozen between updates.
        now = int(self.root.tk.call("clock", "milliseconds"))
        if self.last_frame_ms is None:
            self.last_frame_ms = now
        dt = max(0.010, min(0.050, (now - self.last_frame_ms) / 1000.0))
        self.last_frame_ms = now
        for route in range(4):
            self.spawn_clock[route] += 1
            passed = as_int(self.state, f"passed{route}", 0)
            delta = max(0, passed - self.last_passed[route])
            self.last_passed[route] = passed
            waiting = as_int(self.state, f"waiting{route}", 0)
            active_particles = len([p for p in self.particles if p["route"] == route])
            if self.route_green(route) and waiting > 0 and active_particles < 2 and self.spawn_clock[route] >= 28:
                delta = max(delta, 1)
                self.spawn_clock[route] = 0
            for _ in range(min(delta, 2)):
                self.spawn_particle(route)
        for particle in self.particles:
            particle["progress"] += particle["speed"] * dt
        self.particles = [p for p in self.particles if p["progress"] < 1.08]

    def spawn_particle(self, route):
        self.particles.append({
            "route": route,
            "progress": -0.10,
            "speed": 0.24 + (self.frame % 4) * 0.015,
            "ambulance": as_int(self.state, f"priority{route}", 0) > 0,
            "lane": len(self.particles) % 2,
        })

    def route_green(self, route):
        return as_int(self.state, f"green{route}", 0) == 1

    def draw(self):
        # Full redraw is acceptable for this scale and keeps the UI code simple.
        self.canvas.delete("all")
        self.buttons = []
        width = max(self.canvas.winfo_width(), 1280)
        height = max(self.canvas.winfo_height(), 820)
        self.draw_background(width, height)
        if self.mode == "menu":
            self.draw_menu(width, height)
        else:
            self.draw_simulation(width, height)

    def draw_background(self, width, height):
        self.canvas.create_rectangle(0, 0, width, height, fill=BG, outline="")
        for i in range(0, width, 40):
            self.canvas.create_line(i, 0, i - 180, height, fill="#07111f")
        pulse = 30 + math.sin(self.frame / 40) * 12
        self.canvas.create_oval(-220, -160, 430, 360, fill="#09213b", outline="#11365c", width=2)
        self.canvas.create_oval(width - 380, height - 300, width + 180, height + 220, fill="#101b3b", outline="#243a73", width=2)
        self.canvas.create_oval(width - 210 - pulse, 62 - pulse, width - 210 + pulse, 62 + pulse, outline="#193e63", width=2)

    def draw_menu(self, width, height):
        # Menu screen: fixed two-column layout to avoid overlapping controls.
        self.canvas.create_text(64, 52, anchor="w", text="SMARTCROSS", fill=CYAN, font=("Segoe UI", 34, "bold"))
        self.canvas.create_text(66, 92, anchor="w", text="Traffic control game for OS scheduling, IPC and synchronization", fill=MUTED, font=("Segoe UI", 13))
        self.canvas.create_text(width - 64, 70, anchor="e", text="Configure -> Start -> Observe -> Review", fill=TEXT, font=("Segoe UI", 13, "bold"))

        margin = 64
        gap = 34
        top = 138
        bottom = height - 68
        left = margin
        right = int(width * 0.56)
        self.card(left, top, right, bottom, "Simulation Setup")
        y = top + 72
        content_x = left + 34
        content_right = right - 34
        y = self.option_row(content_x, content_right, y, "Mode", "mode", [("Normal", "normal"), ("Ambulance", "priority")])
        y = self.option_row(content_x, content_right, y, "Strategy", "strategy", [("FCFS", "fcfs"), ("Round Robin", "rr"), ("Priority", "priority"), ("SJF", "sjf"), ("Dynamic", "dynamic")])
        y = self.slider_row(content_x, content_right, y, "Cars", "cars", 40, 300, 20)
        y = self.slider_row(content_x, content_right, y, "Processes", "processes", 2, 5, 1)
        y = self.slider_row(content_x, content_right, y, "Threads / route", "threads", 1, 8, 1)
        y = self.slider_row(content_x, content_right, y, "Quantum", "quantum", 1, 12, 1)
        y = self.slider_row(content_x, content_right, y, "Speed", "speed", 1, 10, 1, invert=True)

        self.button(content_x, bottom - 82, content_right, bottom - 28, "START SIMULATION", self.start_simulation, fill=GREEN, text="#04130b", font_size=13)

        px = right + gap
        self.card(px, top, width - 70, bottom, "How To Read The Game")
        info = [
            ("Goal", "Watch the controller behave like an OS scheduler: it chooses which route receives the critical resource, the intersection."),
            ("Cars", "Each car is a task. Waiting cars are ready jobs. Moving cars are currently being served."),
            ("Routes", "Each route is a process. Internal handling uses threads."),
            ("Synchronization", "The center of the intersection is protected by semaphores to avoid collision and deadlock."),
            ("IPC", "The C processes communicate through pipes, message queues and shared memory. This GUI reads the shared live state."),
            ("Tip", "Use Speed 1 or 2 for presentation. Higher speed is useful for benchmarks but harder to watch."),
        ]
        iy = top + 72
        text_width = width - px - 132
        for title, text in info:
            self.canvas.create_text(px + 34, iy, anchor="w", text=title, fill=YELLOW, font=("Segoe UI", 12, "bold"))
            self.canvas.create_text(px + 34, iy + 23, anchor="w", text=text, fill=MUTED, font=("Segoe UI", 10), width=text_width)
            iy += 82
        self.canvas.create_text(70, height - 30, anchor="w", text=self.message, fill=MUTED, font=("Segoe UI", 10))

    def card(self, x0, y0, x1, y1, title):
        self.canvas.create_rectangle(x0 + 8, y0 + 10, x1 + 8, y1 + 10, fill=SHADOW, outline="")
        self.canvas.create_rectangle(x0, y0, x1, y1, fill=CARD, outline="#1b4065", width=2)
        self.canvas.create_rectangle(x0, y0, x1, y0 + 54, fill="#10294a", outline="")
        self.canvas.create_text(x0 + 26, y0 + 28, anchor="w", text=title, fill=TEXT, font=("Segoe UI", 17, "bold"))

    def option_row(self, x, right, y, label, key, options):
        # Button group with wrapping, used for mode and strategy selections.
        self.canvas.create_text(x, y, anchor="w", text=label, fill=TEXT, font=("Segoe UI", 11, "bold"))
        bx = x + 145
        row_start = bx
        for text, value in options:
            selected = self.config[key] == value
            w = max(70, len(text) * 8 + 20)
            if bx + w > right:
                y += 42
                bx = row_start
            self.button(bx, y - 17, bx + w, y + 17, text, lambda k=key, v=value: self.set_config(k, v), fill=GREEN if selected else CARD_2, text=BG if selected else TEXT, font_size=9)
            bx += w + 10
        return y + 56

    def slider_row(self, x, right, y, label, key, min_value, max_value, step, invert=False):
        # Discrete +/- slider. It is easier to use in a presentation than a tiny
        # draggable widget and avoids focus issues on a Canvas-only UI.
        value = self.config[key]
        caption = f"{value} ({'cinematic' if key == 'speed' and value == 1 else 'slow' if key == 'speed' and value <= 3 else 'fast' if key == 'speed' and value >= 7 else 'balanced'})" if key == "speed" else str(value)
        self.canvas.create_text(x, y, anchor="w", text=f"{label}: {caption}", fill=TEXT, font=("Segoe UI", 11, "bold"))
        minus_x = right - 310
        plus_x = right - 38
        bar_x0 = minus_x + 50
        bar_x1 = plus_x - 14
        self.button(minus_x, y - 17, minus_x + 38, y + 17, "-", lambda k=key: self.bump(k, -step, min_value, max_value), fill=CARD_2)
        self.canvas.create_rectangle(bar_x0, y - 5, bar_x1, y + 5, fill="#07111f", outline="#23415f")
        pct = (value - min_value) / max(1, max_value - min_value)
        if invert:
            fill_pct = 1.0 - pct
        else:
            fill_pct = pct
        self.canvas.create_rectangle(bar_x0, y - 5, bar_x0 + (bar_x1 - bar_x0) * fill_pct, y + 5, fill=CYAN, outline="")
        self.button(plus_x, y - 17, plus_x + 38, y + 17, "+", lambda k=key: self.bump(k, step, min_value, max_value), fill=CARD_2)
        return y + 50

    def button(self, x0, y0, x1, y1, label, action, fill=CARD_2, text=TEXT, font_size=10):
        self.canvas.create_rectangle(x0, y0, x1, y1, fill=fill, outline="#2b5a80", width=2)
        self.canvas.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=label, fill=text, font=("Segoe UI", font_size, "bold"))
        self.buttons.append((x0, y0, x1, y1, action))

    def set_config(self, key, value):
        self.config[key] = value
        if key == "mode" and value == "priority":
            self.config["strategy"] = "priority"

    def bump(self, key, delta, min_value, max_value):
        self.config[key] = max(min_value, min(max_value, self.config[key] + delta))

    def draw_simulation(self, width, height):
        # Simulation screen: left side is the intersection, right side explains
        # what the controller is doing and shows live metrics.
        self.draw_header(width)
        self.draw_intersection(width, height)
        self.draw_control_room(width, height)
        self.draw_bottom_bar(width, height)

    def draw_header(self, width):
        strategy = self.state.get("strategy", self.config["strategy"])
        elapsed = as_float(self.state, "elapsed", 0.0)
        self.canvas.create_text(42, 34, anchor="w", text="SMARTCROSS LIVE", fill=CYAN, font=("Segoe UI", 25, "bold"))
        self.canvas.create_text(44, 66, anchor="w", text=self.message, fill=MUTED, font=("Segoe UI", 11), width=max(520, width * 0.56))
        self.canvas.create_text(width - 42, 44, anchor="e", text=f"{strategy.upper()}  |  {elapsed:05.2f}s", fill=TEXT, font=("Consolas", 14, "bold"))

    def draw_intersection(self, width, height):
        # Keep the intersection inside a safe left region so it never collides
        # with the control-room panel on the right.
        panel_left = int(width * 0.68)
        left_area = panel_left - 52
        cx = int(left_area * 0.50)
        cy = int(height * 0.54)
        road_w = 150
        horiz_h = 146
        extent = min(470, int(left_area * 0.48))
        self.canvas.create_rectangle(cx - road_w // 2, cy - extent, cx + road_w // 2, cy + extent, fill=ROAD, outline=ROAD_EDGE, width=2)
        self.canvas.create_rectangle(cx - extent, cy - horiz_h // 2, cx + extent, cy + horiz_h // 2, fill=ROAD, outline=ROAD_EDGE, width=2)
        self.canvas.create_rectangle(cx - road_w // 2, cy - horiz_h // 2, cx + road_w // 2, cy + horiz_h // 2, fill="#2a3c50", outline="#5a7592", width=3)
        self.draw_lane_markings(cx, cy, road_w, horiz_h, extent)
        self.draw_crosswalks(cx, cy, road_w, horiz_h)
        self.draw_stop_lines(cx, cy, road_w, horiz_h)
        self.draw_traffic_lights(cx, cy, road_w, horiz_h)
        self.draw_route_labels(cx, cy, extent)
        self.draw_queues(cx, cy)
        self.draw_particles(cx, cy)
        self.draw_center_annotation(cx, cy)

    def draw_lane_markings(self, cx, cy, road_w, horiz_h, extent):
        for x in range(cx - extent, cx + extent, 42):
            if not (cx - road_w // 2 < x < cx + road_w // 2):
                self.canvas.create_line(x, cy, x + 20, cy, fill=ROAD_LINE, width=2)
        for y in range(cy - extent, cy + extent, 42):
            if not (cy - horiz_h // 2 < y < cy + horiz_h // 2):
                self.canvas.create_line(cx, y, cx, y + 20, fill=ROAD_LINE, width=2)

    def draw_crosswalks(self, cx, cy, road_w, horiz_h):
        for offset in [-120, 120]:
            for i in range(-3, 4):
                self.canvas.create_rectangle(cx - road_w // 2 + i * 23, cy + offset - 5, cx - road_w // 2 + i * 23 + 14, cy + offset + 5, fill="#dce7f5", outline="")
                self.canvas.create_rectangle(cx + offset - 5, cy - horiz_h // 2 + i * 23, cx + offset + 5, cy - horiz_h // 2 + i * 23 + 14, fill="#dce7f5", outline="")

    def draw_stop_lines(self, cx, cy, road_w, horiz_h):
        # Thick red stop bars make it obvious where cars are waiting on red.
        line_specs = [
            (cx - road_w // 2, cy - horiz_h // 2 - 28, cx + road_w // 2, cy - horiz_h // 2 - 28, 0),
            (cx - road_w // 2, cy + horiz_h // 2 + 28, cx + road_w // 2, cy + horiz_h // 2 + 28, 1),
            (cx + road_w // 2 + 28, cy - horiz_h // 2, cx + road_w // 2 + 28, cy + horiz_h // 2, 2),
            (cx - road_w // 2 - 28, cy - horiz_h // 2, cx - road_w // 2 - 28, cy + horiz_h // 2, 3),
        ]
        for x0, y0, x1, y1, route in line_specs:
            color = GREEN if self.route_green(route) else STOP_LINE
            self.canvas.create_line(x0, y0, x1, y1, fill=color, width=5)

    def draw_traffic_lights(self, cx, cy, road_w, horiz_h):
        positions = {
            0: (cx - road_w // 2 - 36, cy - horiz_h // 2 - 52),
            1: (cx + road_w // 2 + 36, cy + horiz_h // 2 + 52),
            2: (cx + road_w // 2 + 54, cy - horiz_h // 2 - 36),
            3: (cx - road_w // 2 - 54, cy + horiz_h // 2 + 36),
        }
        for route, (x, y) in positions.items():
            green = self.route_green(route)
            self.canvas.create_rectangle(x - 24, y - 42, x + 24, y + 42, fill="#101925", outline="#79a7cc" if green else "#456987", width=3)
            self.canvas.create_oval(x - 14, y - 31, x + 14, y - 3, fill="#42151a" if green else RED, outline="")
            self.canvas.create_oval(x - 14, y + 8, x + 14, y + 36, fill=GREEN if green else "#123321", outline="")
            self.canvas.create_text(x, y + 56, text="GO" if green else "STOP", fill=GREEN if green else RED, font=("Segoe UI", 9, "bold"))
            if green:
                self.canvas.create_oval(x - 36, y - 54, x + 36, y + 54, outline=GREEN, width=2)

    def draw_route_labels(self, cx, cy, extent):
        labels = {0: (cx, cy - extent + 42), 1: (cx, cy + extent - 42), 2: (cx + extent - 90, cy), 3: (cx - extent + 90, cy)}
        for route, (x, y) in labels.items():
            fill = GREEN if self.route_green(route) else MUTED
            self.canvas.create_text(x, y, text=ROUTES[route].upper(), fill=fill, font=("Segoe UI", 17, "bold"))

    def draw_queues(self, cx, cy):
        for route in range(4):
            waiting = as_int(self.state, f"waiting{route}", 0)
            priority = as_int(self.state, f"priority{route}", 0)
            departing = self.departing_cars(route)
            count = min(max(0, waiting - departing), 9)
            for i in range(count):
                queue_index = i + departing
                ambulance = priority > 0 and i == 0
                color = MAGENTA if ambulance else WAITING_CAR
                if route == 0:
                    x, y, angle = cx - 40, cy - 144 - queue_index * 28, 90
                elif route == 1:
                    x, y, angle = cx + 40, cy + 144 + queue_index * 28, -90
                elif route == 2:
                    x, y, angle = cx + 150 + queue_index * 32, cy - 40, 180
                else:
                    x, y, angle = cx - 150 - queue_index * 32, cy + 40, 0
                self.draw_car(x, y, angle, color, ambulance)

    def departing_cars(self, route):
        # Hide the first queued car while its matching green particle is leaving
        # the stop line. This makes it look like the same car starts moving,
        # instead of a new green car appearing elsewhere.
        return min(2, sum(1 for p in self.particles if p["route"] == route and p["progress"] < 0.22))

    def draw_particles(self, cx, cy):
        # Draw smooth vehicle particles over the road after queued cars, so cars
        # that are currently being served visually appear in motion.
        for p in self.particles:
            route = p["route"]
            progress = max(0.0, min(1.0, p["progress"]))
            lane = -38 if p["lane"] == 0 else 38
            if route == 0:
                x, y, angle = cx - 40, cy - 144 + progress * 430, 90
            elif route == 1:
                x, y, angle = cx + 40, cy + 144 - progress * 430, -90
            elif route == 2:
                x, y, angle = cx + 150 - progress * 490, cy - 40, 180
            else:
                x, y, angle = cx - 150 + progress * 490, cy + 40, 0
            color = MAGENTA if p["ambulance"] else GREEN
            self.draw_trail(x, y, angle, color)
            self.draw_car(x, y, angle, color, p["ambulance"])

    def draw_trail(self, x, y, angle, color):
        rad = math.radians(angle)
        dx = math.cos(rad) * 20
        dy = math.sin(rad) * 20
        trail = [color, "#1f7a55" if color == GREEN else "#74368f", "#174d3b" if color == GREEN else "#4f2a66"]
        for i, trail_color in enumerate(trail):
            self.canvas.create_line(x - dx * (i + 1), y - dy * (i + 1), x - dx * (i + 1.6), y - dy * (i + 1.6), fill=trail_color, width=4)

    def draw_car(self, x, y, angle, color, ambulance=False):
        length = 34 if not ambulance else 43
        width = 18 if not ambulance else 22
        rad = math.radians(angle)
        cos_a = math.cos(rad)
        sin_a = math.sin(rad)
        points = []
        for px, py in [(-length / 2, -width / 2), (length / 2, -width / 2), (length / 2, width / 2), (-length / 2, width / 2)]:
            points.extend([x + px * cos_a - py * sin_a, y + px * sin_a + py * cos_a])
        outline = "#ffffff" if color in (GREEN, MAGENTA) else "#c7d3df"
        self.canvas.create_polygon(points, fill=color, outline=outline, width=1)
        self.canvas.create_oval(x - 5, y - 5, x + 5, y + 5, fill="#ffffff" if ambulance else "#07111c", outline="")
        if ambulance:
            self.canvas.create_text(x, y, text="+", fill=RED, font=("Segoe UI", 14, "bold"))

    def draw_center_annotation(self, cx, cy):
        green = as_int(self.state, "current_green", -1)
        strategy = self.state.get("strategy", self.config["strategy"]).upper()
        text = ROUTE_EXPLAIN.get(green, "Controller is waiting for enough vehicle state to make a decision.")
        self.canvas.create_rectangle(cx - 150, cy - 42, cx + 150, cy + 42, fill="#091421", outline="#42617c", width=2)
        self.canvas.create_text(cx, cy - 14, text=f"Scheduler: {strategy}", fill=TEXT, font=("Segoe UI", 12, "bold"))
        self.canvas.create_text(cx, cy + 12, text=text, fill=MUTED, font=("Segoe UI", 8), width=270)

    def draw_control_room(self, width, height):
        # The right panel is split into non-overlapping boxes: explanation,
        # metrics, queues, events, and OS concepts.
        x0 = int(width * 0.68)
        x1 = width - 34
        y0 = 104
        y1 = height - 76
        self.card(x0, y0, x1, y1, "Control Room")
        content_x = x0 + 24
        content_right = x1 - 24
        content_width = content_right - content_x
        self.canvas.create_text(content_x, y0 + 74, anchor="w", text=self.current_explanation(), fill=MUTED, font=("Segoe UI", 9), width=content_width)
        self.draw_legend(content_x, y0 + 116, content_right)
        total = as_int(self.state, "total_cars", self.config["cars"])
        passed = int(self.display_values.get("passed", as_int(self.state, "total_passed", 0)))
        self.draw_progress(content_x, y0 + 176, content_right, passed / total if total else 0, f"{passed}/{total} cars processed")
        metrics = [
            ("Average wait", f"{self.display_values.get('avg_wait', 0):.1f} ms"),
            ("Throughput", f"{self.display_values.get('throughput', 0):.1f} cars/s"),
            ("Collisions avoided", f"{int(self.display_values.get('collisions', 0))}"),
            ("Deadlock retries", f"{int(self.display_values.get('deadlocks', 0))}"),
        ]
        y = y0 + 226
        col_gap = 12
        col_w = (content_width - col_gap) / 2
        for index, (label, value) in enumerate(metrics):
            mx = content_x + (index % 2) * (col_w + col_gap)
            my = y + (index // 2) * 54
            self.metric_card(mx, my, mx + col_w, label, value)
        y += 124

        self.canvas.create_rectangle(content_x, y, content_right, y + 136, fill="#081522", outline="#1d3b59")
        self.canvas.create_text(content_x + 12, y + 18, anchor="w", text="Route queues", fill=TEXT, font=("Segoe UI", 12, "bold"))
        ry = y + 44
        for route, name in enumerate(ROUTES):
            color = GREEN if self.route_green(route) else MUTED
            waiting = as_int(self.state, f"waiting{route}", 0)
            passed_r = as_int(self.state, f"passed{route}", 0)
            self.canvas.create_text(content_x + 14, ry, anchor="w", text=name, fill=color, font=("Segoe UI", 10, "bold"))
            self.canvas.create_text(content_right - 14, ry, anchor="e", text=f"wait {waiting} | passed {passed_r}", fill=TEXT, font=("Consolas", 9))
            ry += 22
        y += 154

        events_h = min(156, max(112, y1 - y - 168))
        self.canvas.create_rectangle(content_x, y, content_right, y + events_h, fill="#081522", outline="#1d3b59")
        self.canvas.create_text(content_x + 12, y + 18, anchor="w", text="Live events", fill=TEXT, font=("Segoe UI", 12, "bold"))
        ey = y + 44
        if not self.events:
            self.canvas.create_text(content_x + 12, ey, anchor="w", text="Waiting for events...", fill=MUTED, font=("Segoe UI", 9))
        max_events = max(2, int((events_h - 48) / 20))
        for event in list(self.events)[:max_events]:
            color = MAGENTA if "AMBULANCE" in event or "ambulance" in event else MUTED
            self.canvas.create_text(content_x + 12, ey, anchor="w", text=event[:52], fill=color, font=("Segoe UI", 8))
            ey += 20

        concepts_y = y + events_h + 16
        if concepts_y + 126 <= y1 - 14:
            self.draw_concepts_box(content_x, concepts_y, content_right)

    def draw_legend(self, x0, y, x1):
        self.canvas.create_rectangle(x0, y, x1, y + 42, fill="#081522", outline="#1d3b59")
        items = [
            (WAITING_CAR, "waiting/stopped"),
            (GREEN, "moving on green"),
            (MAGENTA, "ambulance"),
            (STOP_LINE, "red stop line"),
        ]
        x = x0 + 12
        for color, label in items:
            self.canvas.create_rectangle(x, y + 14, x + 14, y + 28, fill=color, outline="")
            self.canvas.create_text(x + 20, y + 21, anchor="w", text=label, fill=MUTED, font=("Segoe UI", 8))
            x += 118

    def draw_concepts_box(self, x0, y, x1):
        self.canvas.create_rectangle(x0, y, x1, y + 126, fill="#081522", outline="#1d3b59")
        self.canvas.create_text(x0 + 12, y + 18, anchor="w", text="OS concepts", fill=TEXT, font=("Segoe UI", 12, "bold"))
        lines = [
            "Processes: controller + route workers",
            "Threads: concurrent vehicle handling",
            "IPC: pipes, messages, shared memory",
            "Semaphores: protected conflict zones",
        ]
        for index, line in enumerate(lines):
            self.canvas.create_text(x0 + 12, y + 44 + index * 19, anchor="w", text=line, fill=MUTED, font=("Segoe UI", 8))

    def current_explanation(self):
        green = as_int(self.state, "current_green", -1)
        if as_int(self.state, "ambulance_seen", 0):
            return "Ambulance priority is active: normal scheduling is preempted to reduce emergency delay."
        if green >= 0:
            return f"The controller selected {ROUTES[green]}. This is the scheduler choosing which queue gets the critical section."
        return "Route processes are generating cars. The controller reads shared memory and waits for a safe phase."

    def draw_progress(self, x0, y, x1, progress, label):
        progress = max(0.0, min(1.0, progress))
        self.canvas.create_text(x0, y, anchor="w", text=label, fill=MUTED, font=("Segoe UI", 10))
        self.canvas.create_rectangle(x0, y + 18, x1, y + 36, fill="#06111d", outline="#23415f")
        self.canvas.create_rectangle(x0, y + 18, x0 + (x1 - x0) * progress, y + 36, fill=GREEN, outline="")

    def metric_card(self, x0, y, x1, label, value):
        self.canvas.create_rectangle(x0, y, x1, y + 42, fill=CARD_2, outline="#1d3b59")
        self.canvas.create_text(x0 + 10, y + 13, anchor="w", text=label, fill=MUTED, font=("Segoe UI", 8))
        self.canvas.create_text(x0 + 10, y + 30, anchor="w", text=value, fill=TEXT, font=("Segoe UI", 11, "bold"))

    def draw_bottom_bar(self, width, height):
        running = self.mode == "running"
        status = "RUNNING" if running else "COMPLETE"
        color = GREEN if running else YELLOW
        self.canvas.create_rectangle(0, height - 56, width, height, fill="#050b13", outline="#142a40")
        self.canvas.create_text(38, height - 28, anchor="w", text=status, fill=color, font=("Segoe UI", 12, "bold"))
        if self.mode == "finished":
            self.button(width - 320, height - 46, width - 170, height - 12, "BACK TO SETUP", self.back_to_menu, fill=YELLOW, text="#140f00")
        else:
            self.button(width - 320, height - 46, width - 170, height - 12, "STOP", self.stop_simulation, fill=RED, text="#190005")
        self.button(width - 150, height - 46, width - 32, height - 12, "CLOSE", self.close, fill=CARD_2)
        self.canvas.create_text(width - 350, height - 28, anchor="e", text="Processes + threads + IPC + semaphores visualized as traffic", fill=MUTED, font=("Segoe UI", 10))


def main():
    root = tk.Tk()
    SmartCrossGame(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
