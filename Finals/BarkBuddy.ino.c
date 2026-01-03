include <WiFi.h> 
#include <DHT.h> 
#include <driver/i2s.h> 
// --- 1. WIFI SETTINGS --- 
const char* ssid = "Mesh"; 
const char* password = "Deniselean1234"; 
// --- 2. N8N CONFIGURATION --- 
const char* n8n_host = "192.168.5.117"; 
const int n8n_port = 5678; 
const char* n8n_path = "/webhook/bark-buddy"; 
 
// --- 3. HARDWARE PIN DEFINITIONS --- 
#define DHTPIN 4 
#define DHTTYPE DHT11 
 
// I2S Microphone Config (INMP441) 
#define I2S_WS 33 
#define I2S_SD 32 
#define I2S_SCK 14 
#define I2S_PORT I2S_NUM_0 
 
// --- 4. AUDIO SETTINGS --- 
#define SAMPLE_RATE 16000       // 16kHz is good for voice/bark detection 
#define BITS_PER_SAMPLE 16      // 16-bit audio 
#define RECORD_TIME_SECONDS 20  // How long each file should be 
 
// --- VOLUME BOOSTER --- 
#define VOLUME_GAIN 8  // <--- CHANGE THIS: 1=Normal, 4=Loud, 8=Very Loud, 
16=Super Loud 
 
// Buffer for sending data chunks 
#define DMA_BUF_LEN 512 
int16_t sBuffer[DMA_BUF_LEN]; 
 
DHT dht(DHTPIN, DHTTYPE); 
WiFiClient client; 
 
// WAV Header Structure 
struct WAV_HEADER { 
  char riff[4];        // "RIFF" 
  uint32_t flength;    // file length in bytes 
  char wave[4];        // "WAVE" 
  char fmt[4];         // "fmt " 
  uint32_t chunk_size; // size of FMT chunk 
  uint16_t format_tag; // 1 = PCM 
  uint16_t num_chans;  // 1 = Mono 
  uint32_t srate;      // Sample Rate 
  uint32_t bytes_per_sec; 
  uint16_t bytes_per_samp; 
  uint16_t bits_per_samp; 
  char data[4];        // "data" 
  uint32_t dlength;    // data length in bytes 
}; 
 
void setup() { 
  Serial.begin(115200); 
 
  // 1. Start DHT 
  dht.begin(); 
 
  // 2. Start I2S Audio 
  i2s_install(); 
  i2s_setpin(); 
  i2s_start(I2S_PORT); 
 
  // 3. Connect to WiFi 
  Serial.print("Connecting to WiFi"); 
  WiFi.begin(ssid, password); 
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  } 
  Serial.println("\nWiFi Connected!"); 
  Serial.println("Starting Continuous Audio Stream..."); 
} 
 
void loop() { 
  // Check if WiFi is still connected 
  if (WiFi.status() != WL_CONNECTED) { 
    Serial.println("WiFi lost, reconnecting..."); 
    WiFi.reconnect(); 
    return; 
  } 
 
  streamAudioToN8N(); 
  // Loop immediately restarts after function returns 
} 
 
// --------------------------------------------------------- 
// CORE FUNCTION: Stream Audio directly to HTTP 
// --------------------------------------------------------- 
void streamAudioToN8N() { 
  
  // 1. Read Sensors 
  float t = dht.readTemperature(); 
  float h = dht.readHumidity(); 
  
  if (isnan(t)) t = 0; 
  if (isnan(h)) h = 0; 
 
  Serial.printf("New Stream: Temp: %.1f, Hum: %.1f\n", t, h); 
 
  // 2. Connect to Server 
  if (!client.connect(n8n_host, n8n_port)) { 
    Serial.println("Connection to n8n failed!"); 
    delay(1000); 
    return; 
  } 
 
  // 3. Calculate File Size 
  uint32_t audioDataSize = SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * 1 * 
RECORD_TIME_SECONDS; 
  uint32_t totalFileSize = audioDataSize + sizeof(WAV_HEADER); 
 
  // 4. Construct HTTP POST Request 
  String url = String(n8n_path) + "?temp=" + String(t) + "&hum=" + 
String(h); 
 
  client.println("POST " + url + " HTTP/1.1"); 
  client.println("Host: " + String(n8n_host)); 
  client.println("Content-Type: audio/wav"); 
  client.println("Content-Length: " + String(totalFileSize)); 
  client.println("Connection: close"); 
  client.println(); // End of headers 
 
  // 5. Send WAV Header 
  WAV_HEADER header; 
  memcpy(header.riff, "RIFF", 4); 
  header.flength = totalFileSize - 8; 
  memcpy(header.wave, "WAVE", 4); 
  memcpy(header.fmt, "fmt ", 4); 
  header.chunk_size = 16; 
  header.format_tag = 1;       // PCM 
  header.num_chans = 1;        // Mono 
  header.srate = SAMPLE_RATE; 
  header.bytes_per_sec = SAMPLE_RATE * (BITS_PER_SAMPLE / 8); 
  header.bytes_per_samp = (BITS_PER_SAMPLE / 8); 
  header.bits_per_samp = BITS_PER_SAMPLE; 
  memcpy(header.data, "data", 4); 
  header.dlength = audioDataSize; 
 
  client.write((uint8_t*)&header, sizeof(WAV_HEADER)); 
 
  // 6. STREAMING LOOP (The "Pipeline") 
  unsigned long streamStart = millis(); 
  size_t bytesRead = 0; 
  uint32_t totalBytesSent = 0; 
 
  while (totalBytesSent < audioDataSize) { 
    // Read from Mic 
    i2s_read(I2S_PORT, &sBuffer, sizeof(sBuffer), &bytesRead, 
portMAX_DELAY); 
    
    // --- NEW: DIGITAL GAIN (Volume Boost) --- 
    if (bytesRead > 0) { 
      // Calculate how many samples we just read (2 bytes per sample) 
      int samplesRead = bytesRead / 2; 
 
      for (int i = 0; i < samplesRead; i++) { 
        // Multiply by Gain Factor (cast to int32 to prevent overflow 
during math) 
        int32_t boosted = sBuffer[i] * VOLUME_GAIN; 
 
        // CLAMP: Ensure we don't exceed the 16-bit limits (-32768 to 
32767) 
        // If we don't do this, the sound will wrap around and screech. 
        if (boosted > 32767) boosted = 32767; 
        if (boosted < -32768) boosted = -32768; 
 
        // Assign back to buffer 
        sBuffer[i] = (int16_t)boosted; 
      } 
      
      // Write the BOOSTED buffer to WiFi 
      client.write((const uint8_t*)sBuffer, bytesRead); 
      totalBytesSent += bytesRead; 
    } 
    // ---------------------------------------- 
 
    // Safety timeout 
    if (millis() - streamStart > (RECORD_TIME_SECONDS * 1000) + 2000) { 
      Serial.println("Stream timeout!"); 
      break; 
    } 
  } 
 
  // 7. Close Connection 
  client.stop(); 
  Serial.println("Stream finished. File sent to n8n."); 
} 
 
// --------------------------------------------------------- 
// I2S SETUP 
// --------------------------------------------------------- 
void i2s_install() { 
  const i2s_config_t i2s_config = { 
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), 
    .sample_rate = SAMPLE_RATE, 
    .bits_per_sample = i2s_bits_per_sample_t(BITS_PER_SAMPLE), 
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
    .communication_format = I2S_COMM_FORMAT_I2S, 
    .intr_alloc_flags = 0, 
    .dma_buf_count = 8, 
    .dma_buf_len = DMA_BUF_LEN, 
    .use_apll = false 
  }; 
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL); 
} 
 
void i2s_setpin() { 
  const i2s_pin_config_t pin_config = { 
    .bck_io_num = I2S_SCK, 
    .ws_io_num = I2S_WS, 
    .data_out_num = -1, 
    .data_in_num = I2S_SD 
  }; 
  i2s_set_pin(I2S_PORT, &pin_config); 
} 