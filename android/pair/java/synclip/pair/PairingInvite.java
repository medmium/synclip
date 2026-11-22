package synclip.pair;

import android.net.InetAddresses;
import android.net.Uri;

import java.net.URI;
import java.net.URISyntaxException;
import java.util.HashMap;
import java.util.Map;

final class PairingInvite {
    final String host;
    final int port;
    final String device;
    final String key;

    private PairingInvite(String host, int port, String device, String key) {
        this.host = host;
        this.port = port;
        this.device = device;
        this.key = key;
    }

    static PairingInvite parse(String value) {
        if (value.length() > 512) {
            return null;
        }
        final URI uri;
        try {
            uri = new URI(value);
        } catch (URISyntaxException error) {
            return null;
        }
        if (!"synclip".equals(uri.getScheme()) || !"pair".equals(uri.getRawAuthority())
                || !"".equals(uri.getRawPath()) || uri.getRawFragment() != null
                || uri.getRawQuery() == null) {
            return null;
        }
        String[] parameters = uri.getRawQuery().split("&", -1);
        if (parameters.length != 5) {
            return null;
        }
        Map<String, String> fields = new HashMap<>();
        for (String parameter : parameters) {
            String[] field = parameter.split("=", 2);
            if (field.length != 2 || fields.put(field[0], field[1]) != null) {
                return null;
            }
        }
        String host = Uri.decode(fields.get("host"));
        String portText = fields.get("port");
        String device = fields.get("id");
        String key = fields.get("key");
        if (!"3".equals(fields.get("v")) || host == null
                || !host.matches("[0-9a-fA-F:.]{1,45}") || !InetAddresses.isNumericAddress(host)
                || portText == null || !portText.matches("[0-9]{1,5}")
                || device == null || !device.matches("[0-9a-f]{32}")
                || key == null || !key.matches("[0-9a-f]{64}")) {
            return null;
        }
        int port = Integer.parseInt(portText);
        return port > 0 && port <= 65535 ? new PairingInvite(host, port, device, key) : null;
    }

    String address() {
        return (host.indexOf(':') >= 0 ? "[" + host + "]" : host) + ":" + port;
    }
}
