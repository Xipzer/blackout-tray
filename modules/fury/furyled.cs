// furyled: sets Kingston FURY DDR5 RGB through the FURY CTRL service's local WebSocket (ws://127.0.0.1:55599),
// so nothing touches the SMBus directly. Usage: furyled off | furyled RRGGBB
using System;
using System.IO;
using System.Linq;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

static class FuryLed
{
    const string Pass = "3m23s45i599";

    static string Encrypt(string text)
    {
        byte[] salt = new byte[32], iv = new byte[32];
        using (var rng = new RNGCryptoServiceProvider()) { rng.GetBytes(salt); rng.GetBytes(iv); }
        byte[] key = new Rfc2898DeriveBytes(Pass, salt, 1000).GetBytes(32);
        using (var r = new RijndaelManaged { BlockSize = 256, Mode = CipherMode.CBC, Padding = PaddingMode.PKCS7 })
        using (var ms = new MemoryStream())
        {
            using (var cs = new CryptoStream(ms, r.CreateEncryptor(key, iv), CryptoStreamMode.Write))
            {
                byte[] b = Encoding.UTF8.GetBytes(text);
                cs.Write(b, 0, b.Length);
            }
            return Convert.ToBase64String(salt.Concat(iv).Concat(ms.ToArray()).ToArray());
        }
    }

    static string Decrypt(string b64)
    {
        byte[] all = Convert.FromBase64String(b64);
        byte[] key = new Rfc2898DeriveBytes(Pass, all.Take(32).ToArray(), 1000).GetBytes(32);
        using (var r = new RijndaelManaged { BlockSize = 256, Mode = CipherMode.CBC, Padding = PaddingMode.PKCS7 })
        using (var cs = new CryptoStream(new MemoryStream(all.Skip(64).ToArray()), r.CreateDecryptor(key, all.Skip(32).Take(32).ToArray()), CryptoStreamMode.Read))
        using (var sr = new StreamReader(cs, Encoding.UTF8))
            return sr.ReadToEnd();
    }

    static string Slot(int i, string rgb, int brightness)
    {
        return "\"slot_" + i + "\":{\"index\":" + i + ",\"mode\":\"static_color\",\"ctrl_mode\":\"ctrl_color\",\"multicolor\":false," +
            "\"reset_default_effect\":false,\"reset_color_table\":false,\"direction\":0,\"ir_delay\":0,\"speed\":60,\"brightness\":" + brightness +
            ",\"width\":0,\"hue\":0,\"led_number\":0,\"power_saving\":false,\"number_colors\":1,\"color_table\":[[" + rgb +
            "]],\"background_color\":[0,0,0]}";
    }

    static string Send(ClientWebSocket ws, string json)
    {
        byte[] msg = Encoding.UTF8.GetBytes(Encrypt(json));
        ws.SendAsync(new ArraySegment<byte>(msg), WebSocketMessageType.Text, true, CancellationToken.None).Wait(5000);
        var buf = new byte[1 << 16];
        var sb = new StringBuilder();
        WebSocketReceiveResult res;
        do
        {
            var t = ws.ReceiveAsync(new ArraySegment<byte>(buf), CancellationToken.None);
            if (!t.Wait(8000)) return null;
            res = t.Result;
            sb.Append(Encoding.UTF8.GetString(buf, 0, res.Count));
        } while (!res.EndOfMessage);
        return Decrypt(sb.ToString());
    }

    static int Main(string[] args)
    {
        string arg = args.Length > 0 ? args[0].ToLowerInvariant() : "off";
        string rgb = "0,0,0";
        int brightness = 0;
        if (arg != "off")
        {
            int c = Convert.ToInt32(arg, 16);
            rgb = ((c >> 16) & 255) + "," + ((c >> 8) & 255) + "," + (c & 255);
            brightness = 100;
        }
        try
        {
            using (var ws = new ClientWebSocket())
            {
                if (!ws.ConnectAsync(new Uri("ws://127.0.0.1:55599/"), CancellationToken.None).Wait(5000)) return 2;
                // "keep" makes the service re-apply this after sleep/resume instead of its default rainbow.
                Send(ws, "{\"root\":{\"api\":\"set_keep_status\",\"keep\":\"1\"}}");
                string reply = Send(ws, "{\"root\":{\"api\":\"set_dram_led\",\"ctrl_settings_ddr5\":{" + Slot(0, rgb, brightness) + "," + Slot(1, rgb, brightness) + "}}}");
                Console.WriteLine(reply);
                return reply != null && reply.Contains("\"status\":\"0\"") ? 0 : 1;
            }
        }
        catch (Exception e)
        {
            Console.WriteLine(e.Message);
            return 3;
        }
    }
}
