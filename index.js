import 'dotenv/config';
import express from 'express';
import pino from 'pino';
import { nanoid } from 'nanoid';
import mqtt from 'mqtt';
import { createClient } from '@supabase/supabase-js';
import twilio from 'twilio';
import ExpressWs from 'express-ws';

const log = pino({ level: process.env.LOG_LEVEL || 'info' });

const {
    PORT = 3000,
    TWILIO_ACCOUNT_SID,
    TWILIO_AUTH_TOKEN,
    TWILIO_PHONE_NUMBER,
    SUPABASE_URL,
    SUPABASE_SERVICE_KEY,
    MQTT_URL,
    MQTT_USER,
    MQTT_PASS,
    MQTT_CMD_TEMPLATE = 'devices/{device_id}/cmd',
    MQTT_RESP_WILDCARD = 'devices/+/resp',
    READ_TIMEOUT_MS = '5000',
} = process.env;

if (!SUPABASE_URL || !SUPABASE_SERVICE_KEY) {
    throw new Error('Missing SUPABASE_URL or SUPABASE_SERVICE_KEY');
}
if (!MQTT_URL || !MQTT_USER || !MQTT_PASS) {
    log.warn('MQTT creds/URL missing — set MQTT_URL, MQTT_USER, MQTT_PASS');
}

const client = twilio(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
const supabase = createClient(SUPABASE_URL, SUPABASE_SERVICE_KEY);

///////////////////// MQTT client + wait-for-response /////////////////////
const mqttClient = mqtt.connect(MQTT_URL, {
    username: MQTT_USER,
    password: MQTT_PASS,
    // For dev with self-signed EMQX on LAN you can set:
    // rejectUnauthorized: false
});
////////////////////////////////////////////////////////////////////////////

const pending = new Map(); // req_id -> { resolve, reject, timer }


///////////////////// MQTT listeners /////////////////////
mqttClient.on('connect', () => {
    log.info({ url: MQTT_URL }, 'MQTT connected');
    mqttClient.subscribe(MQTT_RESP_WILDCARD, (err) => {
        if (err) log.error({ err }, 'MQTT subscribe error');
    });
});

mqttClient.on('error', (err) => {
    log.error({ err }, 'MQTT client error');
});

mqttClient.on('message', (topic, payload) => {
    try {
        const msg = JSON.parse(payload.toString()); // ex payload: {"req_id":"t1","device":"kit-001","temp_c":-127,"temp_f":-196.6,"ts":1448}
        const { req_id } = msg || {};
        if (req_id && pending.has(req_id)) {
            const { resolve, timer } = pending.get(req_id);
            clearTimeout(timer);
            pending.delete(req_id);
            resolve({ topic, msg });
        }
    } catch (e) {
        log.warn({ e }, 'Bad MQTT JSON on %s', topic);
    }
});

function publishCmd(device_id, cmdBody) {
    const topic = MQTT_CMD_TEMPLATE.replace('{device_id}', device_id);
    mqttClient.publish(topic, JSON.stringify(cmdBody));
}

function requestLiveReading(device_id, timeoutMs = Number(READ_TIMEOUT_MS)) {
    return new Promise((resolve, reject) => {
        const req_id = nanoid();
        const timer = setTimeout(() => {
            pending.delete(req_id);
            reject(new Error('timeout'));
        }, timeoutMs); // 5s
        pending.set(req_id, { resolve, reject, timer });
        publishCmd(device_id, { cmd: 'read_temp', req_id }); // ex cmd: { "cmd": "read_temp", "req_id": "t1" }
    });
}
///////////////////////////////////////////////////////////////



////// Express app + wss /////////////////
const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
ExpressWs(app);

const connections = new Map(); // to remember caller's phone number
//////////////////////////////////////

// Health check
app.get('/health', (_req, res) => res.json({ ok: true }));


///////////// Temoerature intent detector /////////////
function isAskTemperature(text) {
    if (!text) return false;
    const t = text.toLowerCase();
    return (
        t.includes("what's the temperature") ||
        t.includes('whats the temperature') ||
        t.includes('what is the temperature') ||
        t.includes('temperature') ||
        t.includes('temp')
    );
}
//////////////////////////////////////////////////////


//////////// Supabase query / live read //////////////
async function handleTurn(fromPhone, asrText) {

    console.log("from phone:", fromPhone)
    // Lookup device by caller phone
    const { data: attendee, error: aerr } = await supabase
        .from('attendees')
        .select('device_id')
        .eq('phone', fromPhone)
        .maybeSingle();


    console.log(attendee, aerr);

    if (aerr) {
        log.error({ aerr }, 'Supabase attendees lookup error');
        return "Sorry, I had trouble looking up your sensor.";
    }
    if (!attendee?.device_id) {
        return "I don't see a registered sensor for this phone. Please register first.";
    }
    const device_id = attendee.device_id;

    if (!isAskTemperature(asrText)) {
        return 'You can ask, “What is the temperature?” to read your sensor live.';
    }

    // Try live read via MQTT
    try {
        const { msg } = await requestLiveReading(device_id);
        const f = Number(msg?.temp_f);
        if (Number.isFinite(f)) {
            // cache latest
            await supabase.from('readings_latest').upsert({
                device_id,
                temp_f: f,
                temp_c: msg?.temp_c ?? null,
                updated_at: new Date().toISOString(),
            });
            return `Your sensor ${device_id} reads ${f.toFixed(1)} degrees Fahrenheit.`;
        }
        throw new Error('non-numeric reading');
    } catch (e) {
        log.warn({ e: String(e), device_id }, 'Live reading failed; using cache');
        // Fallback to cached value
        const { data: last } = await supabase
            .from('readings_latest')
            .select('temp_f, updated_at')
            .eq('device_id', device_id)
            .maybeSingle();
        if (last?.temp_f != null) {
            const f = Number(last.temp_f);
            return `I couldn't get a live reading. The most recent value for ${device_id} was ${f.toFixed(1)} degrees Fahrenheit.`;
        }
        return `I couldn't get a live reading and I don't have any cached data yet for ${device_id}. Make sure your kit is on Wi-Fi.`;
    }
}
//////////////////////////////////////////////////////

/**
 * Twilio Conversation Relay
 * Docs (Onboarding): https://www.twilio.com/docs/voice/conversationrelay/onboarding
 * Types of messages from CR: https://www.twilio.com/docs/voice/conversationrelay/websocket-messages 
 */
app.ws('/twilio-wss-for-conversation-relay', async (ws) => {
    try {

        ws.on('message', async (data) => {
            const msg = JSON.parse(data);
            console.log("Incoming message:", msg);

            // Set phone number in ws var
            if (msg.from) {
                connections.set(ws, msg.from);
            }

            if (msg.type === "prompt") {
                console.log("Prompt received:", msg);
                const phoneNum = connections.get(ws);

                try {
                    const reply = await handleTurn(phoneNum, msg.voicePrompt);

                    ws.send(JSON.stringify({
                        type: "text",
                        token: reply
                    }));
                } catch (e) {
                    console.log("Error @ grabbing IoT data:", e);
                }
            }
        });

        ws.on('close', () => {
            connections.delete(ws)
            log.info('CR WebSocket closed');
        });

        ws.on('error', (err) => {
            log.error({ err }, 'CR WebSocket error');
        });
    } catch (err) {
        console.log(err);
    }

});


// Graceful shutdown
function shutdown(sig) {
    log.info({ sig }, 'Shutting down…');
    try { mqttClient.end(true); } catch { }
    process.exit(0);
}
process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));

app.listen(Number(PORT), () => {
    log.info({ port: PORT }, 'CR server listening');
});
