import serial
import time
import sys
import numpy as np
from ultralytics import YOLO
import cv2
import math
import requests
import os
import shutil
import msvcrt

YOLO_MODEL_PATH = "../Yolo-Weights/yolov8m.pt"
DELAY_BETWEEN_CYCLES = 5.0 # Time to wait after one full scan before starting the next
ser = None
DELAY_BETWEEN_TURNS = 3.0 # Time to pause after a turn before starting next action

CAMERA_URL = "http://192.168.180.242/cam-hi.jpg"

MAX_DISPLAY_WIDTH = 1280
MAX_DISPLAY_HEIGHT = 720
DISPLAY_DURATION_MS = 3000 # 3 seconds per image during final playback

raw_images_dir = "temp_raw_images" # New directory for raw captured images
annotated_images_dir = "temp_annotated" # Directory for annotated images
saved_annotated_image_paths = [] # To store paths of final annotated images

# initialize YOLO model
model = None

# class names that YOLO can detect
classNames = ["person", "bicycle", "car", "motorbike", "aeroplane", "bus", "train", "truck", "boat",
              "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
              "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
              "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite", "baseball bat",
              "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
              "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli",
              "carrot", "hot dog", "pizza", "donut", "cake", "chair", "sofa", "pottedplant", "bed",
              "diningtable", "toilet", "tvmonitor", "laptop", "mouse", "remote", "keyboard", "cell phone",
              "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors",
              "teddy bear", "hair drier", "toothbrush"
              ]

# class names we want to detect and colors
vehicle_classes = ["car", "truck", "bus", "motorbike"]
vehicle_colors = {
    "car": (0, 255, 0), # green
    "truck": (255, 0, 0), # blue
    "bus": (0, 0, 255), # red
    "motorbike": (0, 255, 255) #yellow
 }
default_color = (128, 128, 128) # Grey for other detections or fallback


def setup_arduino_connection():
    """Establishes serial connection to Arduino for motor control."""
    global ser
    while True:
        arduino_port = input("Arduino's COM Port (like COM6): ").strip()
        if arduino_port:
            try:
                ser = serial.Serial(arduino_port, 9600, timeout=1)
                time.sleep(2)
                print(f"Connected to arduino on {arduino_port}")
                initial_msg = ser.read_all().decode('utf-8', errors='ignore').strip()
                if initial_msg:
                    print(f"Arduino says: {initial_msg}")
                break
            except serial.SerialException as e:
                print(f"Error: Could not open port {arduino_port}. {e}")
    return True

def setup_yolo_and_dirs():
    """Initializes YOLO and temporary directories. Removed SORT initialization."""
    global model
    try:
        model = YOLO(YOLO_MODEL_PATH)
        print("YOLO model loaded.")
    except Exception as e:
        print(f"FATAL: Error loading YOLO model from {YOLO_MODEL_PATH}: {e}")
        return False

    # directories for raw and annotated images
    os.makedirs(raw_images_dir, exist_ok=True)
    os.makedirs(annotated_images_dir, exist_ok=True)
    return True

def send_arduino_command(command):
    """Sends a command to the Arduino motor controller."""
    if ser and ser.is_open:
        try:
            ser.write(f"{command}\n".encode('utf-8'))
        except Exception as e:
            print(f"Error sending command '{command}': {e}")
    else:
        print("Arduino serial connection not open.")

def read_arduino_response():
    """Reads a single line from the Arduino."""
    if ser and ser.in_waiting > 0:
        try:
            line = ser.readline().decode('utf-8').strip()
            if line:
                return line
        except Exception as e:
            print(f"Error reading from Arduino serial: {e}")
    return None

def capture_image_from_esp32(url):
    """Fetches a single frame from the ESP32-CAM via WiFi."""
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()

        # Convert the byte array to a numpy array, then decode as an image
        img_array = np.array(bytearray(response.content), dtype=np.uint8)
        img = cv2.imdecode(img_array, -1) # -1 means read as is (color or grayscale)

        if img is None:
            print("Warning: Could not decode image from ESP32-CAM. Received corrupt data?")
        return img
    except requests.exceptions.Timeout:
        print(f"Error: Request to ESP32-CAM timed out after 10 seconds. Check connection.")
        return None
    except requests.exceptions.RequestException as e:
        print(f"Error fetching image from ESP32-CAM at {url}: {e}")
        return None
    except Exception as e:
        print(f"An unexpected error occurred during image capture: {e}")
    return None


def process_and_annotate_image(img, side_num):
    """
    Processes a single image:
    1. Runs YOLO detection.
    2. Annotates the original resolution image with boxes and labels using standard OpenCV.
    3. Saves the annotated image to 'temp_annotated'.
    Returns the number of vehicles found by simply counting the detected boxes.
    """
    if img is None or img.size == 0:
        print(f"Error: No valid image provided for processing Side {side_num}.")
        return 0

    print(f"Running YOLO detection on Side {side_num} image...")

    img_annotated_original_res = img.copy() # Make a copy to draw on

    if img_annotated_original_res is None or img_annotated_original_res.size == 0:
        print(f"Error: Image copy failed or resulted in empty image for side {side_num}. Cannot annotate.")
        return 0

    detected_vehicle_count = 0

    results = model(img, stream=True, verbose=False)

    for r in results:
        if img_annotated_original_res is None or img_annotated_original_res.size == 0:
            print(f"Critical Error: Image became invalid during detection loop for side {side_num}. Aborting annotation for this image.")
            break 

        for box in r.boxes:
            x1_orig, y1_orig, x2_orig, y2_orig = map(int, box.xyxy[0])
            conf = math.ceil((box.conf[0] * 100)) / 100
            cls = int(box.cls[0])
            currentClass = classNames[cls]

            # Filter for vehicle classes with sufficient confidence
            if currentClass in vehicle_classes and conf > 0.3:
                detected_vehicle_count += 1 # Increment count directly here
                
                box_color = vehicle_colors.get(currentClass, default_color)
                thickness = 2 

                if img_annotated_original_res is None or img_annotated_original_res.size == 0:
                    print(f"Warning: Image became invalid before drawing a box on side {side_num}. Skipping further annotations for this image.")
                    break 
                    
                cv2.rectangle(img_annotated_original_res, (x1_orig, y1_orig), (x2_orig, y2_orig), box_color, thickness)

                label = f"{currentClass} {conf:.2f}"
                font = cv2.FONT_HERSHEY_SIMPLEX
                font_scale = 0.8
                font_thickness = 2
                text_color = (255, 255, 255) 

                (text_width, text_height), baseline = cv2.getTextSize(label, font, font_scale, font_thickness)
                
                text_bg_y1 = max(0, y1_orig - text_height - baseline)
                text_bg_y2 = y1_orig
                cv2.rectangle(img_annotated_original_res, (x1_orig, text_bg_y1), (x1_orig + text_width, text_bg_y2), box_color, cv2.FILLED)
                
                text_y_pos = max(text_height + baseline, y1_orig - baseline)
                cv2.putText(img_annotated_original_res, label, (x1_orig, text_y_pos), font, font_scale, text_color, font_thickness, cv2.LINE_AA)
        
        if img_annotated_original_res is None or img_annotated_original_res.size == 0:
            break 

    # Add vehicle count to the top left corner
    if img_annotated_original_res is not None and img_annotated_original_res.size > 0:
        cv2.putText(img_annotated_original_res, f'Vehicles: {detected_vehicle_count}', (25, 50),
                            cv2.FONT_HERSHEY_PLAIN, 2, (197, 246, 246), 2, cv2.LINE_AA)
    else:
        print(f"Warning: Annotated image became invalid. Cannot add vehicle count text for side {side_num}.")
        return 0

    # Save the annotated image
    annotated_image_filename = os.path.join(annotated_images_dir, f"side_{side_num}_annotated_{int(time.time())}.jpg")
    cv2.imwrite(annotated_image_filename, img_annotated_original_res)
    saved_annotated_image_paths.append(annotated_image_filename)
    print(f"Annotated image saved to: {annotated_image_filename}")

    return detected_vehicle_count


def resize_for_display(img):
    """Resizes an image to fit within MAX_DISPLAY_WIDTH and MAX_DISPLAY_HEIGHT."""
    original_height, original_width = img.shape[:2]
    new_width = original_width
    new_height = original_height

    if original_width > MAX_DISPLAY_WIDTH:
        new_width = MAX_DISPLAY_WIDTH
        new_height = int(original_height * (new_width / original_width))
    if new_height > MAX_DISPLAY_HEIGHT:
        new_height = MAX_DISPLAY_HEIGHT
        new_width = int(original_width * (new_height / original_width))
        
    return cv2.resize(img, (new_width, new_height))


def calibration_mode():
    """Allows manual adjustment of the stepper motor with a continuous live camera view."""
    print("\n--- Calibration Mode ---")
    print("Live view is active.")
    print("INSTRUCTIONS:")
    print(" - Type directly in this CONSOLE window.")
    print(" - Move motor: 'm <steps>' (e.g., 'm 50' or 'm -200') and press Enter.")
    print(" - To exit: type 'done' and press Enter, or press 'q' in the Live View window.")

    send_arduino_command("P")  # Put Arduino in pause/idle mode
    time.sleep(0.5)
    response = read_arduino_response()
    if response:
        print(f"Arduino: {response}")

    cv2.namedWindow('Calibration - Live View', cv2.WINDOW_NORMAL)
    command_buffer = ""

    while True:
        # --- Handle Live View ---
        img = capture_image_from_esp32(CAMERA_URL)
        if img is None:
            print("\rFailed to get image, retrying...", end="")
            time.sleep(1)
            continue

        img = cv2.rotate(img, cv2.ROTATE_180)
        if img is None:
            print("\rError: Rotated image is None.", end="")
            continue

        display_img = resize_for_display(img.copy())
        # Display the current command being typed on the live view
        cv2.putText(display_img, f"CMD: {command_buffer}", (20, 50), 
                    cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 255), 2, cv2.LINE_AA)
        cv2.imshow('Calibration - Live View', display_img)

        # --- Handle OpenCV Window Input ---
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            print("\n'q' pressed in window. Exiting calibration.")
            break

        # --- Handle Non-Blocking Console Input ---
        if msvcrt.kbhit():
            char = msvcrt.getch()
            try:
                decoded_char = char.decode('utf-8')
                # Handle Enter key
                if decoded_char == '\r' or decoded_char == '\n':
                    print() # Newline after command is entered
                    command_to_process = command_buffer.strip().lower()
                    command_buffer = "" # Clear buffer

                    if command_to_process == 'done':
                        print("Exiting calibration.")
                        break
                    elif command_to_process.startswith('m '):
                        parts = command_to_process.split()
                        if len(parts) == 2:
                            try:
                                steps = int(parts[1])
                                send_arduino_command(f"M{steps}")
                                print(f">>> Sent 'M{steps}' to Arduino. Waiting for response...")
                                time.sleep(0.1)
                                response = read_arduino_response()
                                if response:
                                    print(f"Arduino: {response}")
                                else:
                                    print("No immediate response from Arduino.")
                            except ValueError:
                                print("ERROR: Invalid steps value. Please enter an integer.")
                        else:
                            print("ERROR: Invalid 'm' command. Use 'm <steps>'.")
                    elif command_to_process:
                        print(f"ERROR: Unknown command '{command_to_process}'.")
                    
                    print("\nEnter command: ", end="", flush=True)

                # Handle backspace
                elif decoded_char == '\x08': 
                    if len(command_buffer) > 0:
                        command_buffer = command_buffer[:-1]
                        # Erase character from console
                        print('\b \b', end="", flush=True)
                else:
                    command_buffer += decoded_char
                    print(decoded_char, end="", flush=True)

            except UnicodeDecodeError:
                pass # Ignore non-UTF-8 characters

    cv2.destroyWindow('Calibration - Live View')
    print("--- Exited Calibration Mode ---")


def run_continuous_scan_loop():
    """
    Runs the full scan process in a continuous, alternating-direction loop.
    Exits when perform_full_scan() returns False (user pressed ESC or q).
    """
    print("\n--- Starting Automatic Continuous Scan Mode ---")
    print(">>> To stop, press 'ESC' or 'q' during the image playback at the end of a cycle. <<<")
    time.sleep(3)

    cycle_count = 1
    is_clockwise = False # <--- NEW: Start with a counter-clockwise scan

    while True:
        # <--- MODIFIED: Pass the direction flag to the scan function ---
        print(f"\n==================== STARTING SCAN CYCLE #{cycle_count} ====================")
        should_continue = perform_full_scan(is_clockwise)
        
        if not should_continue:
            print("\nUser requested to stop. Returning to main menu.")
            break
        
        print(f"==================== CYCLE #{cycle_count} COMPLETE ====================")
        print(f"Pausing for {DELAY_BETWEEN_CYCLES} seconds before starting the next cycle...")
        time.sleep(DELAY_BETWEEN_CYCLES)
        
        # <--- NEW: Flip the direction for the next cycle ---
        is_clockwise = not is_clockwise
        cycle_count += 1

def perform_full_scan(is_clockwise=False):
    """
    Performs a 4-sided scan, controlling the stepper motor and capturing images.
    The direction of scan and side numbering depends on the is_clockwise flag.
    """
    if not ser or not ser.is_open:
        print("Error: Arduino not connected. Cannot perform scan.")
        return True # Return True to allow trying again

    # <--- MODIFIED: Determine direction and print status ---
    direction_str = "Clockwise (Reverse)" if is_clockwise else "Counter-Clockwise (Forward)"
    print(f"\nStarting full 360-degree scan (Image Capture Phase)... Direction: {direction_str}")
    
    # Clear previous saved images in both directories
    if os.path.exists(raw_images_dir):
        shutil.rmtree(raw_images_dir)
    os.makedirs(raw_images_dir, exist_ok=True)
    
    if os.path.exists(annotated_images_dir):
        shutil.rmtree(annotated_images_dir)
    os.makedirs(annotated_images_dir, exist_ok=True)

    saved_annotated_image_paths.clear()
    captured_raw_images_data = []

    for i in range(4):
        # Determine the side number for the CURRENT camera position
        current_side = (4 - i) if is_clockwise else (i + 1)

        # 1. Capture and process the image at the CURRENT position
        print(f"\n--- Capturing image for Side {current_side} ---")
        img = capture_image_from_esp32(CAMERA_URL)

        if img is None:
            print(f"Failed to capture image for Side {current_side}. This side will have 0 vehicles.")
            captured_raw_images_data.append((current_side, None))
        else:
            img = cv2.rotate(img, cv2.ROTATE_180)
            if img is None:
                print(f"Error: Rotated image is None for Side {current_side}.")
                captured_raw_images_data.append((current_side, None))
            else:
                print(f"Image for Side {current_side} captured and rotated.")
                raw_image_filename = os.path.join(raw_images_dir, f"side_{current_side}_raw_{int(time.time())}.jpg")
                cv2.imwrite(raw_image_filename, img)
                print(f"Raw image saved to: {raw_image_filename}")
                captured_raw_images_data.append((current_side, img))

        # 2. If it's not the last side, turn to the NEXT position
        if i < 3:
            turn_command = 'R' if is_clockwise else 'T'
            print(f"\n--- Turning to the next position ---")
            send_arduino_command(turn_command)

            print("Waiting for motor to finish turning (max 10 seconds)...")
            turn_done = False
            start_wait = time.time()
            while time.time() - start_wait < 10:
                response = read_arduino_response()
                if response == "TURN_DONE":
                    print("Motor turn complete.")
                    turn_done = True
                    break
                elif response:
                    print(f"Arduino: {response}")
                time.sleep(0.1)

            if not turn_done:
                print(f"Warning: Timed out waiting for 'TURN_DONE' from Arduino. Proceeding.")

            print(f"Waiting for {DELAY_BETWEEN_TURNS}s before next capture...")
            time.sleep(DELAY_BETWEEN_TURNS)

    print("\n--- Image Capture Phase Complete. Starting Analysis Phase ---")
    
    total_vehicles = 0
    side_vehicle_counts = {}

    for side_num, img_data in captured_raw_images_data:
        if img_data is not None:
            vehicle_count = process_and_annotate_image(img_data, side_num)
            side_vehicle_counts[side_num] = vehicle_count
            total_vehicles += vehicle_count
            print(f"--- Side {side_num} Analysis: Found {vehicle_count} vehicles. ---")
        else:
            print(f"Skipping analysis for Side {side_num} due to capture failure. Count set to 0.")
            side_vehicle_counts[side_num] = 0

    print("\n--- Full Scan Analysis Complete ---")
    
    # The sorting logic remains the same and works perfectly regardless of scan order
    sorted_sides = sorted(side_vehicle_counts.items(), key=lambda item: item[1], reverse=True)

    print("\n--- Vehicle Counts per Side ---")
    for side, count in sorted_sides:
        print(f"Side {side}: {count} vehicles")
    print(f"\nTotal Vehicles Detected Across All Sides: {total_vehicles}")

    priority_order_str = "".join([str(side) for side, count in sorted_sides])
    
    if priority_order_str:
        priority_command = "O" + priority_order_str
        print(f"--- Decision: Sending priority command to Arduino: '{priority_command}' ---")
        send_arduino_command(priority_command)
        print("Note: The Arduino will apply this new priority after the current signal cycle.")
    else:
        print("No vehicle counts available to determine a priority order.")
    
    # --- Playback Annotated Images (this part is from the previous change and remains the same) ---
    # It returns True/False based on user input (ESC or q)
    print("\n--- Starting Playback of Annotated Images ---")
    should_continue_looping = True

    if not saved_annotated_image_paths:
        print("No annotated images to display.")
    else:
        cv2.namedWindow("Vehicle Detection Playback", cv2.WINDOW_NORMAL)
        for img_path in saved_annotated_image_paths:
            # ... (The rest of the playback logic is unchanged)
            # It will correctly set should_continue_looping to False if ESC/q is pressed.
            # NO CHANGES NEEDED HERE from the previous step.
            print(f"Displaying: {img_path}")
            img_to_display = cv2.imread(img_path)

            if img_to_display is None:
                print(f"Error: Could not load saved annotated image '{img_path}' for display. Skipping.")
                continue

            original_height, original_width = img_to_display.shape[:2]
            new_width = original_width
            new_height = original_height

            if original_width > MAX_DISPLAY_WIDTH:
                new_width = MAX_DISPLAY_WIDTH
                new_height = int(original_height * (new_width / original_width))
            if new_height > MAX_DISPLAY_HEIGHT:
                new_height = MAX_DISPLAY_HEIGHT
                new_width = int(original_width * (new_height / original_width))
                
            img_to_display_resized = cv2.resize(img_to_display, (new_width, new_height))

            cv2.imshow("Vehicle Detection Playback", img_to_display_resized)
            
            key = cv2.waitKey(DISPLAY_DURATION_MS) & 0xFF
            if key == 27 or key == ord('q'):
                print("Stop request detected during playback. Signaling to exit continuous mode.")
                should_continue_looping = False
                break

        if cv2.getWindowProperty("Vehicle Detection Playback", cv2.WND_PROP_VISIBLE) >= 0:
            cv2.destroyWindow("Vehicle Detection Playback")

    return should_continue_looping

# --- Cleanup Function ---

def cleanup():
    """Cleans up resources before exiting."""
    print("\nCleaning up and exiting...")
    if ser and ser.is_open:
        ser.close()
    print("Arduino serial connection closed.")
    cv2.destroyAllWindows() # This will close all OpenCV windows
    print("OpenCV windows closed.")
    
    # Clean up temporary directories
    if os.path.exists(raw_images_dir):
        try:
            shutil.rmtree(raw_images_dir)
            print(f"Removed raw images directory: {raw_images_dir}")
        except Exception as e:
            print(f"Error removing raw images directory {raw_images_dir}: {e}")
    
    if os.path.exists(annotated_images_dir):
        try:
            shutil.rmtree(annotated_images_dir)
            print(f"Removed annotated images directory: {annotated_images_dir}")
        except Exception as e:
            print(f"Error removing annotated images directory {annotated_images_dir}: {e}")


# --- Main Program Flow ---

def main():
    """Main function to run the program."""
    print("--- 360-Degree Vehicle Detection with Stepper Motor ---")
    
    if not setup_arduino_connection():
        cleanup()
        return

    if not setup_yolo_and_dirs():
        cleanup()
        return

    try:
        while True:
            print("\n--- Main Menu ---")
            print(" (S) Start Automatic Continuous Scan")
            print(" (C) Enter Calibration Mode")
            print(" (Q) Quit")
            choice = input("Enter your choice: ").strip().upper()

            if choice == 'S':
                run_continuous_scan_loop() # Call the new continuous loop function
            elif choice == 'C':
                calibration_mode()
            elif choice == 'Q':
                break
            else:
                print("Invalid choice.")

    except KeyboardInterrupt:
        print("\nProgram interrupted by user (Ctrl+C).")
    finally:
        cleanup()

if __name__ == "__main__":
    main()