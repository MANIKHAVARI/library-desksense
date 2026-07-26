# This application receives HTTP and CoAP telemetry,
# receives MQTT messages, and stores all data in InfluxDB.

import asyncio
import json
import os
import threading
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import aiocoap
import aiocoap.resource as resource
import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS


INFLUX_URL = os.environ.get("INFLUX_URL")
INFLUX_TOKEN = os.environ.get("INFLUX_TOKEN")
INFLUX_ORG = os.environ.get("INFLUX_ORG")
INFLUX_BUCKET = os.environ.get("INFLUX_BUCKET")

HTTP_HOST = "0.0.0.0"
HTTP_PORT = int(os.environ.get("HTTP_PORT", "18080"))

COAP_HOST = os.environ.get("COAP_HOST", "0.0.0.0")
COAP_PORT = int(os.environ.get("COAP_PORT", "5683"))

# The backend normally connects to a broker on the same machine.
# The ESP32 must use the machine's LAN address in project_config.h.
MQTT_BROKER = os.environ.get("MQTT_BROKER", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))

MQTT_EVENT_TOPIC = "library/desk1/event"
MQTT_STATUS_TOPIC = "library/desk1/status"
MQTT_CONFIG_TOPIC = "library/desk1/config/#"
MQTT_METRICS_TOPIC = "library/desk1/metrics"

FORECAST_WINDOW = 12
FORECAST_MIN_SAMPLES = 5
ANALYTICS_WINDOW = 60
ANALYTICS_WRITE_INTERVAL = 10
BACKEND_VERSION = "3.0-analytics"


# This function checks that all required InfluxDB settings exist.
def check_influx_settings():
    missing = []

    if not INFLUX_URL:
        missing.append("INFLUX_URL")

    if not INFLUX_TOKEN:
        missing.append("INFLUX_TOKEN")

    if not INFLUX_ORG:
        missing.append("INFLUX_ORG")

    if not INFLUX_BUCKET:
        missing.append("INFLUX_BUCKET")

    if missing:
        raise RuntimeError(
            "Missing environment variables: " + ", ".join(missing)
        )


check_influx_settings()

influx_client = InfluxDBClient(
    url=INFLUX_URL,
    token=INFLUX_TOKEN,
    org=INFLUX_ORG,
)

write_api = influx_client.write_api(
    write_options=SYNCHRONOUS
)

influx_write_lock = threading.Lock()
analytics_lock = threading.Lock()

light_history = deque(maxlen=FORECAST_WINDOW)
noise_history = deque(maxlen=FORECAST_WINDOW)
telemetry_history = deque(maxlen=ANALYTICS_WINDOW)

pending_forecast = None
analytics_sample_counter = 0

forecast_error_totals = {
    "light_absolute": 0.0,
    "light_squared": 0.0,
    "noise_absolute": 0.0,
    "noise_squared": 0.0,
    "count": 0,
}


# This function writes one point into InfluxDB.
def write_point(point):
    with influx_write_lock:
        write_api.write(
            bucket=INFLUX_BUCKET,
            org=INFLUX_ORG,
            record=point,
        )


# This function returns the slope of a simple linear trend.
def calculate_linear_slope(values):
    count = len(values)

    if count < 2:
        return 0.0

    x_mean = (count - 1) / 2.0
    y_mean = sum(values) / count

    numerator = 0.0
    denominator = 0.0

    for index, value in enumerate(values):
        x_difference = index - x_mean
        numerator += x_difference * (value - y_mean)
        denominator += x_difference * x_difference

    if denominator == 0:
        return 0.0

    return numerator / denominator


# This function predicts the next value using a linear trend.
def predict_next_value(values):
    if not values:
        return 0.0

    if len(values) == 1:
        return float(values[-1])

    slope = calculate_linear_slope(values)
    average = sum(values) / len(values)
    center = (len(values) - 1) / 2.0

    return average + slope * (len(values) - center)


# This function evaluates the previous forecast using the newest sample.
def evaluate_pending_forecast(clean_telemetry):
    global pending_forecast

    if pending_forecast is None:
        return

    actual_light = float(clean_telemetry["light"])
    actual_noise = float(clean_telemetry["sound_edges"])

    predicted_light = pending_forecast["predicted_light"]
    predicted_noise = pending_forecast[
        "predicted_sound_edges"
    ]

    light_error = abs(actual_light - predicted_light)
    noise_error = abs(actual_noise - predicted_noise)

    light_squared_error = light_error ** 2
    noise_squared_error = noise_error ** 2

    forecast_error_totals["light_absolute"] += light_error
    forecast_error_totals["light_squared"] += (
        light_squared_error
    )
    forecast_error_totals["noise_absolute"] += noise_error
    forecast_error_totals["noise_squared"] += (
        noise_squared_error
    )
    forecast_error_totals["count"] += 1

    count = forecast_error_totals["count"]

    light_mae = (
        forecast_error_totals["light_absolute"] / count
    )
    light_mse = (
        forecast_error_totals["light_squared"] / count
    )
    noise_mae = (
        forecast_error_totals["noise_absolute"] / count
    )
    noise_mse = (
        forecast_error_totals["noise_squared"] / count
    )

    point = (
        Point("forecast_accuracy")
        .tag("desk_id", "desk1")
        .tag("model", "linear_trend")
        .tag("protocol", pending_forecast["protocol"])
        .field("predicted_light", predicted_light)
        .field("actual_light", actual_light)
        .field("light_absolute_error", light_error)
        .field("light_squared_error", light_squared_error)
        .field("light_mae", light_mae)
        .field("light_mse", light_mse)
        .field(
            "predicted_sound_edges",
            predicted_noise,
        )
        .field("actual_sound_edges", actual_noise)
        .field("noise_absolute_error", noise_error)
        .field("noise_squared_error", noise_squared_error)
        .field("noise_mae", noise_mae)
        .field("noise_mse", noise_mse)
        .field("evaluated_forecasts", count)
    )

    write_point(point)

    print(
        "Forecast evaluated: "
        f"light MAE={light_mae:.2f}, "
        f"light MSE={light_mse:.2f}, "
        f"noise MAE={noise_mae:.2f}, "
        f"noise MSE={noise_mse:.2f}."
    )

    pending_forecast = None


# This function stores a one-sample light and noise forecast.
def store_forecast(protocol):
    global pending_forecast

    if (
        len(light_history) < FORECAST_MIN_SAMPLES
        or len(noise_history) < FORECAST_MIN_SAMPLES
    ):
        return

    predicted_light = predict_next_value(
        list(light_history)
    )
    predicted_noise = predict_next_value(
        list(noise_history)
    )

    predicted_light = max(
        0.0,
        min(4095.0, predicted_light),
    )
    predicted_noise = max(0.0, predicted_noise)

    training_samples = min(
        len(light_history),
        len(noise_history),
    )

    point = (
        Point("forecast_output")
        .tag("desk_id", "desk1")
        .tag("model", "linear_trend")
        .tag("protocol", protocol)
        .field("predicted_light", predicted_light)
        .field(
            "predicted_sound_edges",
            predicted_noise,
        )
        .field("horizon_samples", 1)
        .field("training_samples", training_samples)
    )

    write_point(point)

    pending_forecast = {
        "predicted_light": predicted_light,
        "predicted_sound_edges": predicted_noise,
        "protocol": protocol,
    }

    print(
        "Forecast saved to InfluxDB: "
        f"light={predicted_light:.2f}, "
        f"sound_edges={predicted_noise:.2f}."
    )


# This function stores rolling utilization and environment trends.
def store_rolling_analytics(protocol):
    if len(telemetry_history) < FORECAST_MIN_SAMPLES:
        return

    samples = list(telemetry_history)
    sample_count = len(samples)

    occupied_values = [
        sample["occupied"] for sample in samples
    ]
    light_values = [
        sample["light"] for sample in samples
    ]
    noise_values = [
        sample["sound_edges"] for sample in samples
    ]
    high_noise_values = [
        sample["high_noise"] for sample in samples
    ]
    poor_light_values = [
        sample["poor_light"] for sample in samples
    ]

    utilization_percent = (
        sum(occupied_values) / sample_count * 100.0
    )
    average_light = sum(light_values) / sample_count
    average_noise = sum(noise_values) / sample_count
    high_noise_percent = (
        sum(high_noise_values) / sample_count * 100.0
    )
    poor_light_percent = (
        sum(poor_light_values) / sample_count * 100.0
    )

    point = (
        Point("desk_analytics")
        .tag("desk_id", "desk1")
        .tag("protocol", protocol)
        .field(
            "utilization_percent",
            utilization_percent,
        )
        .field("average_light", average_light)
        .field(
            "average_sound_edges",
            average_noise,
        )
        .field(
            "high_noise_percent",
            high_noise_percent,
        )
        .field(
            "poor_light_percent",
            poor_light_percent,
        )
        .field(
            "light_trend_per_sample",
            calculate_linear_slope(light_values),
        )
        .field(
            "noise_trend_per_sample",
            calculate_linear_slope(noise_values),
        )
        .field("sample_count", sample_count)
    )

    write_point(point)

    print(
        "Rolling analytics saved to InfluxDB: "
        f"utilization={utilization_percent:.1f}%, "
        f"average light={average_light:.1f}, "
        f"average sound edges={average_noise:.1f}."
    )


# This function updates forecasting and rolling analytics.
def process_telemetry_analysis(clean_telemetry, protocol):
    global analytics_sample_counter

    with analytics_lock:
        evaluate_pending_forecast(clean_telemetry)

        light_history.append(clean_telemetry["light"])
        noise_history.append(
            clean_telemetry["sound_edges"]
        )
        telemetry_history.append(dict(clean_telemetry))

        store_forecast(protocol)

        analytics_sample_counter += 1

        if (
            analytics_sample_counter
            % ANALYTICS_WRITE_INTERVAL
            == 0
        ):
            store_rolling_analytics(protocol)


# This function validates and converts telemetry values.
def prepare_telemetry(telemetry):
    if not isinstance(telemetry, dict):
        raise ValueError("Telemetry must be a JSON object.")

    return {
        "occupied": int(telemetry.get("occupied", 0)),
        "occupancy_duration": int(
            telemetry.get("occupancy_duration", 0)
        ),
        "light": int(telemetry.get("light", 0)),
        "poor_light": int(telemetry.get("poor_light", 0)),
        "sound_edges": int(telemetry.get("sound_edges", 0)),
        "high_noise": int(telemetry.get("high_noise", 0)),
    }


# This function stores HTTP or CoAP telemetry.
def save_telemetry(telemetry, protocol):
    clean_telemetry = prepare_telemetry(telemetry)

    point = (
        Point("desk_telemetry")
        .tag("desk_id", "desk1")
        .tag("protocol", protocol)
        .field("occupied", clean_telemetry["occupied"])
        .field(
            "occupancy_duration",
            clean_telemetry["occupancy_duration"],
        )
        .field("light", clean_telemetry["light"])
        .field("poor_light", clean_telemetry["poor_light"])
        .field("sound_edges", clean_telemetry["sound_edges"])
        .field("high_noise", clean_telemetry["high_noise"])
    )

    write_point(point)

    try:
        process_telemetry_analysis(
            clean_telemetry,
            protocol,
        )

    except Exception as error:
        print(f"Analytics processing failed: {error}")


# This function extracts the event name and optional duration.
def parse_mqtt_event(payload):
    event_name = payload
    duration_seconds = None

    try:
        decoded_payload = json.loads(payload)

        if isinstance(decoded_payload, dict):
            event_name = str(
                decoded_payload.get(
                    "event",
                    decoded_payload.get("type", payload),
                )
            )

            if "duration_seconds" in decoded_payload:
                duration_seconds = int(
                    decoded_payload["duration_seconds"]
                )

    except (json.JSONDecodeError, TypeError, ValueError):
        pass

    return event_name, duration_seconds


# This function stores an MQTT event and any completed occupancy session.
def save_mqtt_event(payload):
    event_name, duration_seconds = parse_mqtt_event(payload)

    event_point = (
        Point("desk_event")
        .tag("desk_id", "desk1")
        .tag("source", "mqtt")
        .field("event", event_name)
        .field("payload", payload)
    )

    write_point(event_point)

    if (
        event_name == "desk_released"
        and duration_seconds is not None
        and duration_seconds >= 0
    ):
        session_point = (
            Point("occupancy_session")
            .tag("desk_id", "desk1")
            .tag("source", "mqtt")
            .field("duration_seconds", duration_seconds)
        )

        write_point(session_point)


# This function validates and converts one communication measurement.
def prepare_communication_metric(payload):
    decoded_payload = json.loads(payload)

    if not isinstance(decoded_payload, dict):
        raise ValueError(
            "Communication metric must be a JSON object."
        )

    protocol = str(
        decoded_payload.get("protocol", "")
    ).lower()

    if protocol not in {"http", "coap"}:
        raise ValueError(
            "Communication protocol must be HTTP or CoAP."
        )

    payload_bytes = int(
        decoded_payload.get("payload_bytes", 0)
    )

    overhead_bytes = int(
        decoded_payload.get("overhead_bytes", 0)
    )

    total_bytes = int(
        decoded_payload.get(
            "total_bytes",
            payload_bytes + overhead_bytes,
        )
    )

    latency_ms = int(
        decoded_payload.get("latency_ms", 0)
    )

    success = int(
        decoded_payload.get("success", 0)
    )

    if payload_bytes < 0 or overhead_bytes < 0:
        raise ValueError(
            "Communication byte counts cannot be negative."
        )

    if total_bytes < payload_bytes:
        raise ValueError(
            "Total bytes cannot be smaller than payload bytes."
        )

    if latency_ms < 0:
        raise ValueError(
            "Communication latency cannot be negative."
        )

    if success not in {0, 1}:
        raise ValueError(
            "Communication success must be zero or one."
        )

    efficiency_percent = 0.0

    if total_bytes > 0:
        efficiency_percent = (
            payload_bytes /
            total_bytes *
            100.0
        )

    return {
        "protocol": protocol,
        "latency_ms": latency_ms,
        "payload_bytes": payload_bytes,
        "overhead_bytes": overhead_bytes,
        "total_bytes": total_bytes,
        "success": success,
        "efficiency_percent": efficiency_percent,
    }


# This function stores one HTTP or CoAP communication measurement.
def save_communication_metric(payload):
    metric = prepare_communication_metric(payload)

    point = (
        Point("communication_metrics")
        .tag("desk_id", "desk1")
        .tag("protocol", metric["protocol"])
        .field("latency_ms", metric["latency_ms"])
        .field("payload_bytes", metric["payload_bytes"])
        .field("overhead_bytes", metric["overhead_bytes"])
        .field("total_bytes", metric["total_bytes"])
        .field("success", metric["success"])
        .field(
            "efficiency_percent",
            metric["efficiency_percent"],
        )
    )

    write_point(point)

    return metric


# This function stores an MQTT status message.
def save_mqtt_status(payload):
    point = (
        Point("desk_status")
        .tag("desk_id", "desk1")
        .tag("source", "mqtt")
        .field("status", payload)
    )

    write_point(point)


# This function stores an MQTT configuration message.
def save_mqtt_config(topic, payload):
    config_name = topic.rsplit("/", 1)[-1]

    point = (
        Point("desk_config")
        .tag("desk_id", "desk1")
        .tag("source", "mqtt")
        .tag("config_name", config_name)
        .field("value", payload)
        .field("topic", topic)
    )

    write_point(point)


class TelemetryHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # This method sends a JSON response to the ESP32.
    def send_json_response(self, status_code, response_data):
        response_body = json.dumps(response_data).encode("utf-8")

        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response_body)))
        self.send_header("Connection", "close")
        self.end_headers()

        self.wfile.write(response_body)
        self.wfile.flush()
        self.close_connection = True

    # This method receives HTTP POST telemetry.
    def do_POST(self):
        if self.path != "/telemetry":
            self.send_json_response(
                404,
                {"status": "not found"},
            )
            return

        try:
            content_length = int(
                self.headers.get("Content-Length", "0")
            )

            if content_length <= 0:
                raise ValueError("The HTTP request body is empty.")

            request_body = self.rfile.read(content_length)

            telemetry = json.loads(
                request_body.decode("utf-8")
            )

            print("\nHTTP telemetry received:")
            print(json.dumps(telemetry, indent=2))

            save_telemetry(telemetry, "http")

            print("HTTP telemetry saved to InfluxDB.")

            self.send_json_response(
                200,
                {"status": "received"},
            )

        except (
            json.JSONDecodeError,
            UnicodeDecodeError,
            TypeError,
            ValueError,
        ) as error:
            print(f"Invalid HTTP telemetry: {error}")

            self.send_json_response(
                400,
                {"status": "invalid telemetry"},
            )

        except Exception as error:
            print(f"HTTP processing failed: {error}")

            self.send_json_response(
                500,
                {"status": "server error"},
            )

    # This method disables the default HTTP access log.
    def log_message(self, format, *args):
        return


class CoAPTelemetryResource(resource.Resource):

    # This method receives CoAP POST telemetry.
    async def render_post(self, request):
        try:
            telemetry = json.loads(
                request.payload.decode("utf-8")
            )

            print("\nCoAP telemetry received:")
            print(json.dumps(telemetry, indent=2))

            save_telemetry(telemetry, "coap")

            print("CoAP telemetry saved to InfluxDB.")

            return aiocoap.Message(
                code=aiocoap.CHANGED,
                payload=b"received",
            )

        except (
            json.JSONDecodeError,
            UnicodeDecodeError,
            TypeError,
            ValueError,
        ) as error:
            print(f"Invalid CoAP telemetry: {error}")

            return aiocoap.Message(
                code=aiocoap.BAD_REQUEST,
                payload=b"invalid telemetry",
            )

        except Exception as error:
            print(f"CoAP processing failed: {error}")

            return aiocoap.Message(
                code=aiocoap.INTERNAL_SERVER_ERROR,
                payload=b"server error",
            )


# This function runs the HTTP server in a background thread.
def run_http_server(http_server):
    http_server.serve_forever()


# This function runs after the MQTT proxy connects to Mosquitto.
def on_mqtt_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties=None,
):
    if reason_code == 0:
        print("\nMQTT proxy connected.")

        client.subscribe(MQTT_EVENT_TOPIC, qos=1)
        client.subscribe(MQTT_STATUS_TOPIC, qos=1)
        client.subscribe(MQTT_CONFIG_TOPIC, qos=1)
        client.subscribe(MQTT_METRICS_TOPIC, qos=1)

        print("MQTT proxy subscriptions active.")

    else:
        print(
            "MQTT proxy connection failed: "
            f"{reason_code}"
        )


# This function processes each MQTT message.
def on_mqtt_message(client, userdata, message):
    topic = message.topic

    payload = message.payload.decode(
        "utf-8",
        errors="replace",
    ).strip()

    print("\nMQTT message received:")
    print(f"Topic: {topic}")
    print(f"Payload: {payload}")

    try:
        if topic == MQTT_EVENT_TOPIC:
            save_mqtt_event(payload)
            print("MQTT event saved to InfluxDB.")

            event_name, duration_seconds = parse_mqtt_event(
                payload
            )

            if (
                event_name == "desk_released"
                and duration_seconds is not None
            ):
                print(
                    "Occupancy session saved to InfluxDB: "
                    f"{duration_seconds}s."
                )

        elif topic == MQTT_STATUS_TOPIC:
            save_mqtt_status(payload)
            print("MQTT status saved to InfluxDB.")

        elif topic == MQTT_METRICS_TOPIC:
            metric = save_communication_metric(payload)

            print(
                "COMMUNICATION METRIC SAVED TO INFLUXDB: "
                f"{metric['protocol'].upper()} | "
                f"{metric['latency_ms']} ms | "
                f"{metric['payload_bytes']} B payload | "
                f"{metric['overhead_bytes']} B overhead | "
                f"{metric['total_bytes']} B total | "
                f"{metric['efficiency_percent']:.1f}% efficiency."
            )

        elif topic.startswith("library/desk1/config/"):
            save_mqtt_config(topic, payload)
            print("MQTT configuration saved to InfluxDB.")

    except Exception as error:
        print(f"MQTT processing failed: {error}")


# This function creates and configures the MQTT client.
def create_mqtt_client():
    try:
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="LibraryDeskSenseProxy",
        )

    except AttributeError:
        client = mqtt.Client(
            client_id="LibraryDeskSenseProxy",
        )

    client.on_connect = on_mqtt_connect
    client.on_message = on_mqtt_message

    return client


# This function starts the HTTP, CoAP, and MQTT services.
async def run_services():
    http_server = ThreadingHTTPServer(
        (HTTP_HOST, HTTP_PORT),
        TelemetryHandler,
    )

    http_thread = threading.Thread(
        target=run_http_server,
        args=(http_server,),
        daemon=True,
    )

    http_thread.start()

    mqtt_client = create_mqtt_client()

    mqtt_client.connect(
        MQTT_BROKER,
        MQTT_PORT,
        keepalive=60,
    )

    mqtt_client.loop_start()

    coap_site = resource.Site()

    coap_site.add_resource(
        ["telemetry"],
        CoAPTelemetryResource(),
    )

    coap_context = (
        await aiocoap.Context.create_server_context(
            coap_site,
            bind=(COAP_HOST, COAP_PORT),
        )
    )

    print(
        "LibraryDeskSense backend running. "
        f"Version: {BACKEND_VERSION}"
    )
    print(f"HTTP: TCP port {HTTP_PORT}")
    print(f"CoAP: UDP port {COAP_PORT}")
    print(f"MQTT broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"InfluxDB bucket: {INFLUX_BUCKET}")
    print(
        "Analytics: rolling utilization, light and noise trends."
    )
    print(
        "Forecasting: one-sample linear trend with MAE and MSE."
    )
    print("Press Ctrl+C to stop.")

    try:
        await asyncio.get_running_loop().create_future()

    finally:
        print("\nStopping proxy services...")

        http_server.shutdown()
        http_server.server_close()

        mqtt_client.loop_stop()
        mqtt_client.disconnect()

        await coap_context.shutdown()


# This function starts the application and closes InfluxDB safely.
def main():
    try:
        asyncio.run(run_services())

    except KeyboardInterrupt:
        print("\nLibraryDeskSense backend stopped.")

    finally:
        write_api.close()
        influx_client.close()


if __name__ == "__main__":
    main()
