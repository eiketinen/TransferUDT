# -*- coding: utf-8 -*-
from __future__ import annotations

import math
from pathlib import Path
from textwrap import wrap

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "TransferUDT_Arquitetura_Funcionamento.pdf"

PAGE_W = 1240
PAGE_H = 1754
MARGIN = 82

COLORS = {
    "ink": "#17202A",
    "muted": "#5D6778",
    "line": "#B9C4D0",
    "soft": "#EEF3F8",
    "agent": "#E8F4EA",
    "agent_line": "#2E7D4F",
    "server": "#EAF0FB",
    "server_line": "#315DAB",
    "core": "#FFF3D7",
    "core_line": "#A36E00",
    "secure": "#EAF7F5",
    "secure_line": "#207A73",
    "warn": "#FFF0E8",
    "warn_line": "#B85D25",
    "white": "#FFFFFF",
    "dark": "#203040",
}


def font(name: str, size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = {
        "regular": [
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
        ],
        "bold": [
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/arialbd.ttf",
        ],
        "mono": [
            "C:/Windows/Fonts/consola.ttf",
            "C:/Windows/Fonts/cour.ttf",
        ],
    }
    for candidate in candidates.get(name, candidates["regular"]):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            pass
    return ImageFont.load_default()


F = {
    "title": font("bold", 52),
    "subtitle": font("regular", 29),
    "h1": font("bold", 36),
    "h2": font("bold", 27),
    "h3": font("bold", 22),
    "body": font("regular", 22),
    "small": font("regular", 18),
    "tiny": font("regular", 15),
    "mono": font("mono", 17),
    "mono_small": font("mono", 15),
}


def page(number: int, title: str | None = None) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    img = Image.new("RGB", (PAGE_W, PAGE_H), COLORS["white"])
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, PAGE_W, 24), fill=COLORS["dark"])
    if title:
        draw.text((MARGIN, 48), title, font=F["h1"], fill=COLORS["ink"])
        draw.line((MARGIN, 102, PAGE_W - MARGIN, 102), fill=COLORS["line"], width=2)
    footer = f"TransferUDT - Arquitetura e funcionamento | {number}"
    tw = text_width(draw, footer, F["tiny"])
    draw.text((PAGE_W - MARGIN - tw, PAGE_H - 46), footer, font=F["tiny"], fill=COLORS["muted"])
    return img, draw


def text_width(draw: ImageDraw.ImageDraw, text: str, used_font) -> int:
    box = draw.textbbox((0, 0), text, font=used_font)
    return box[2] - box[0]


def text_height(draw: ImageDraw.ImageDraw, text: str, used_font) -> int:
    box = draw.textbbox((0, 0), text, font=used_font)
    return box[3] - box[1]


def wrap_pixels(draw: ImageDraw.ImageDraw, text: str, used_font, max_width: int) -> list[str]:
    if not text:
        return []
    lines: list[str] = []
    for paragraph in text.split("\n"):
        current = ""
        for word in paragraph.split():
            candidate = word if not current else f"{current} {word}"
            if text_width(draw, candidate, used_font) <= max_width:
                current = candidate
            else:
                if current:
                    lines.append(current)
                    current = word
                else:
                    lines.extend(wrap(word, 18))
                    current = ""
        if current:
            lines.append(current)
    return lines


def draw_wrapped(
    draw: ImageDraw.ImageDraw,
    xy: tuple[int, int],
    text: str,
    used_font,
    fill: str,
    max_width: int,
    line_gap: int = 8,
) -> int:
    x, y = xy
    for line in wrap_pixels(draw, text, used_font, max_width):
        draw.text((x, y), line, font=used_font, fill=fill)
        y += text_height(draw, line, used_font) + line_gap
    return y


def rounded_box(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    fill: str,
    outline: str,
    width: int = 2,
    radius: int = 16,
) -> None:
    draw.rounded_rectangle(rect, radius=radius, fill=fill, outline=outline, width=width)


def box(
    draw: ImageDraw.ImageDraw,
    rect: tuple[int, int, int, int],
    title: str,
    body: str = "",
    fill: str = COLORS["soft"],
    outline: str = COLORS["line"],
    title_font=F["h3"],
    body_font=F["small"],
    title_fill: str = COLORS["ink"],
    body_fill: str = COLORS["muted"],
) -> None:
    x1, y1, x2, y2 = rect
    rounded_box(draw, rect, fill, outline)
    draw.text((x1 + 18, y1 + 15), title, font=title_font, fill=title_fill)
    if body:
        draw_wrapped(draw, (x1 + 18, y1 + 49), body, body_font, body_fill, x2 - x1 - 36, 6)


def arrow(
    draw: ImageDraw.ImageDraw,
    start: tuple[int, int],
    end: tuple[int, int],
    color: str = COLORS["dark"],
    width: int = 4,
    label: str | None = None,
    label_offset: tuple[int, int] = (0, -34),
) -> None:
    draw.line((start, end), fill=color, width=width)
    sx, sy = start
    ex, ey = end
    angle = math.atan2(ey - sy, ex - sx)
    head_len = 18
    head_angle = math.pi / 7
    p1 = (
        ex - head_len * math.cos(angle - head_angle),
        ey - head_len * math.sin(angle - head_angle),
    )
    p2 = (
        ex - head_len * math.cos(angle + head_angle),
        ey - head_len * math.sin(angle + head_angle),
    )
    draw.polygon([end, p1, p2], fill=color)
    if label:
        lx = (sx + ex) // 2 + label_offset[0]
        ly = (sy + ey) // 2 + label_offset[1]
        padding = 7
        tw = text_width(draw, label, F["tiny"])
        th = text_height(draw, label, F["tiny"])
        draw.rounded_rectangle(
            (lx - padding, ly - padding, lx + tw + padding, ly + th + padding),
            radius=8,
            fill=COLORS["white"],
            outline=color,
            width=1,
        )
        draw.text((lx, ly), label, font=F["tiny"], fill=color)


def bullet_list(
    draw: ImageDraw.ImageDraw,
    x: int,
    y: int,
    items: list[str],
    max_width: int,
    used_font=F["body"],
    gap: int = 18,
    bullet_color: str = COLORS["server_line"],
) -> int:
    for item in items:
        draw.ellipse((x, y + 9, x + 10, y + 19), fill=bullet_color)
        new_y = draw_wrapped(draw, (x + 25, y), item, used_font, COLORS["ink"], max_width - 25, 7)
        y = new_y + gap
    return y


def cover_page() -> Image.Image:
    img, draw = page(1)
    draw.rectangle((0, 0, PAGE_W, 500), fill="#F4F8FB")
    draw.rectangle((0, 0, PAGE_W, 24), fill=COLORS["dark"])
    draw.text((MARGIN, 94), "TransferUDT", font=F["title"], fill=COLORS["ink"])
    draw.text((MARGIN, 162), "Arquitetura, funcionamento e caso de uso", font=F["subtitle"], fill=COLORS["muted"])
    draw_wrapped(
        draw,
        (MARGIN, 230),
        "Visão técnica da aplicação de transferência segura por UDT, incluindo componentes, fluxo criptográfico, controles de segurança e uma execução end-to-end.",
        F["body"],
        COLORS["ink"],
        860,
        9,
    )

    agent = (112, 630, 384, 790)
    core = (484, 630, 756, 790)
    server = (856, 630, 1128, 790)
    box(draw, agent, "Agente", "Monitora diretórios, fragmenta arquivos e envia chunks.", COLORS["agent"], COLORS["agent_line"])
    box(draw, core, "Canal seguro", "Handshake autenticado + pacote AES-256-GCM.", COLORS["secure"], COLORS["secure_line"])
    box(draw, server, "Servidor", "Autentica, valida, armazena chunks e reconstrói arquivos.", COLORS["server"], COLORS["server_line"])
    arrow(draw, (384, 710), (484, 710), COLORS["secure_line"], 5, "chunks")
    arrow(draw, (756, 710), (856, 710), COLORS["secure_line"], 5, "ACK autenticado")

    draw.text((MARGIN, 900), "O que este documento cobre", font=F["h1"], fill=COLORS["ink"])
    bullet_list(
        draw,
        MARGIN,
        970,
        [
            "Arquitetura lógica: Agent, TransferCore, Server, SQLite e armazenamento em disco.",
            "Fluxo de autenticação, criptografia, validação de integridade e reconstrução.",
            "Controles adicionados para isolamento por identidade, quotas, filas limitadas e rejeição de links.",
            "Caso de uso passo a passo com resultado esperado e validação end-to-end.",
        ],
        PAGE_W - 2 * MARGIN,
    )
    return img


def architecture_page() -> Image.Image:
    img, draw = page(2, "Arquitetura lógica")
    top = 160
    col_w = 315
    gap = 45
    x_agent = MARGIN
    x_core = x_agent + col_w + gap
    x_server = x_core + col_w + gap

    draw.text((x_agent, top), "AgentUDTC++_v7.2", font=F["h2"], fill=COLORS["agent_line"])
    draw.text((x_core, top), "TransferCore", font=F["h2"], fill=COLORS["core_line"])
    draw.text((x_server, top), "ServerUDTC++_v3", font=F["h2"], fill=COLORS["server_line"])

    agent_boxes = [
        ("FileWatcher", "Observa data.dirs e rejeita links/reparse points."),
        ("FileProcessor", "Calcula hash, divide chunks e controla estabilidade."),
        ("NetworkManager", "Executa handshake, envelope seguro e ACK autenticado."),
        ("SQLite local", "Estado de filas, chunks e entregas confirmadas."),
    ]
    core_boxes = [
        ("SecurityHandshake", "Challenge-response com client_id e HMAC-SHA256."),
        ("SecurePacket", "AES-256-GCM, nonce, tag e metadados autenticados."),
        ("ThreadPool", "Fila com limite para reduzir DoS por conexões pendentes."),
        ("Logger/Config", "Telemetria e parâmetros de segurança compartilhados."),
    ]
    server_boxes = [
        ("ServerUDT", "Aceita conexões UDT e aplica bind/allowlist de origem."),
        ("ClientHandler", "Mantém identidade autenticada durante a sessão."),
        ("FileReceiver", "Valida chunk, quota, hashes e monta arquivo final."),
        ("Database + storage", "Namespace por client_id e persistência de progresso."),
    ]

    y0 = 220
    for i, (title, body) in enumerate(agent_boxes):
        box(draw, (x_agent, y0 + i * 170, x_agent + col_w, y0 + 130 + i * 170), title, body, COLORS["agent"], COLORS["agent_line"])
    for i, (title, body) in enumerate(core_boxes):
        box(draw, (x_core, y0 + i * 170, x_core + col_w, y0 + 130 + i * 170), title, body, COLORS["core"], COLORS["core_line"])
    for i, (title, body) in enumerate(server_boxes):
        box(draw, (x_server, y0 + i * 170, x_server + col_w, y0 + 130 + i * 170), title, body, COLORS["server"], COLORS["server_line"])

    arrow(draw, (x_agent + col_w, 455), (x_core, 455), COLORS["secure_line"], 4, "protocolo")
    arrow(draw, (x_core + col_w, 455), (x_server, 455), COLORS["secure_line"], 4, "sessão")
    arrow(draw, (x_server + 155, 900), (x_server + 155, 1035), COLORS["server_line"], 4, "chunks")

    draw.text((MARGIN, 970), "Modelo de responsabilidade", font=F["h1"], fill=COLORS["ink"])
    bullet_list(
        draw,
        MARGIN,
        1040,
        [
            "O Agent decide o que enviar e nunca deve seguir caminhos fora dos diretórios monitorados.",
            "O TransferCore centraliza regras de segurança reutilizadas pelos dois binários.",
            "O Server é a autoridade de recepção: autentica, aplica quota, grava estado e reconstrói o arquivo.",
        ],
        PAGE_W - 2 * MARGIN,
    )
    return img


def sequence_page() -> Image.Image:
    img, draw = page(3, "Fluxo seguro de transferência")
    lanes = [
        ("Agente", 180, COLORS["agent_line"]),
        ("Servidor", 620, COLORS["server_line"]),
        ("Disco/DB", 980, COLORS["core_line"]),
    ]
    top = 180
    bottom = 1410
    for label, x, color in lanes:
        draw.text((x - 55, 135), label, font=F["h2"], fill=color)
        draw.line((x, top, x, bottom), fill="#D8DEE8", width=3)

    steps = [
        (235, 180, 620, "Conecta via UDT"),
        (330, 620, 180, "AUTH_CHALLENGE"),
        (425, 180, 620, "AUTH_RESPONSE client_id + HMAC"),
        (520, 620, 180, "READY"),
        (635, 180, 620, "SecurePacket AES-GCM"),
        (750, 620, 980, "Valida identidade, nonce, hash e quota"),
        (865, 980, 620, "Chunk persistido"),
        (980, 620, 180, "SUCCESS / FILE_COMPLETE autenticado"),
        (1095, 180, 980, "Registra entrega no SQLite local"),
        (1210, 180, 620, "CLOSE_NOW quando aplicável"),
    ]
    for y, x1, x2, label in steps:
        arrow(draw, (x1, y), (x2, y), COLORS["secure_line"] if x1 != 980 else COLORS["core_line"], 4, label)

    draw.text((MARGIN, 1328), "Propriedade importante", font=F["h2"], fill=COLORS["ink"])
    draw_wrapped(
        draw,
        (MARGIN, 1372),
        "A entrega só é marcada como bem-sucedida depois que a resposta de controle também passa pelo envelope autenticado. Assim, um endpoint falso ou um atacante de rede não consegue forjar SUCCESS sem conhecer a chave da sessão.",
        F["body"],
        COLORS["ink"],
        PAGE_W - 2 * MARGIN,
        8,
    )
    return img


def security_page() -> Image.Image:
    img, draw = page(4, "Controles de segurança e resiliência")
    center = (PAGE_W // 2, 470)
    draw.ellipse((center[0] - 125, center[1] - 125, center[0] + 125, center[1] + 125), fill=COLORS["secure"], outline=COLORS["secure_line"], width=5)
    draw.text((center[0] - 86, center[1] - 24), "Transfer", font=F["h2"], fill=COLORS["secure_line"])
    draw.text((center[0] - 52, center[1] + 10), "seguro", font=F["h2"], fill=COLORS["secure_line"])

    controls = [
        ((90, 180, 420, 315), "Identidade forte", "PSK por cliente e allowlist de client_id.", COLORS["agent"], COLORS["agent_line"]),
        ((820, 180, 1150, 315), "Envelope criptográfico", "AES-256-GCM com chave derivada por HKDF.", COLORS["secure"], COLORS["secure_line"]),
        ((90, 420, 420, 555), "Isolamento de arquivos", "Namespace de storage baseado em client_id autenticado.", COLORS["server"], COLORS["server_line"]),
        ((820, 420, 1150, 555), "Anti-replay", "SessionId autenticado + sequence monotônico.", COLORS["secure"], COLORS["secure_line"]),
        ((90, 660, 420, 795), "Watcher seguro", "Rejeita symlinks/reparse points e valida caminho canônico.", COLORS["warn"], COLORS["warn_line"]),
        ((820, 660, 1150, 795), "ACK autenticado", "SUCCESS e FILE_ALREADY_EXISTS protegidos contra forja.", COLORS["secure"], COLORS["secure_line"]),
        ((90, 900, 420, 1035), "Quotas", "Limite por arquivo, por cliente e por armazenamento.", COLORS["warn"], COLORS["warn_line"]),
        ((820, 900, 1150, 1035), "Fila limitada", "ThreadPool recusa tarefas quando atinge capacidade.", COLORS["warn"], COLORS["warn_line"]),
    ]
    for rect, title, body, fill, outline in controls:
        box(draw, rect, title, body, fill, outline)
        sx = (rect[0] + rect[2]) // 2
        sy = rect[3] if rect[1] < center[1] else rect[1]
        arrow(draw, (sx, sy), center, outline, 3)

    draw.text((MARGIN, 1165), "Risco residual conhecido", font=F["h1"], fill=COLORS["ink"])
    bullet_list(
        draw,
        MARGIN,
        1235,
        [
            "A autenticação usa PSK/HMAC, não cadeia de certificados ou mTLS.",
            "A sessão rejeita replay, troca de sessão e ordem inválida; a idempotência reduz duplicidade operacional.",
            "Parâmetros de quota, allowlist e chaves por cliente precisam ser tratados como configuração de produção.",
        ],
        PAGE_W - 2 * MARGIN,
    )
    return img


def use_case_page() -> Image.Image:
    img, draw = page(5, "Caso de uso passo a passo")
    draw_wrapped(
        draw,
        (MARGIN, 138),
        "Cenário: um agente em uma estação de produção envia um arquivo gerado localmente para um servidor central, preservando integridade, identidade e isolamento por cliente.",
        F["body"],
        COLORS["ink"],
        PAGE_W - 2 * MARGIN,
        8,
    )

    steps = [
        ("1", "Configurar agente", "Definir data.dirs, client_id, PSK do cliente, endereço do servidor e limites de chunk."),
        ("2", "Detectar arquivo", "FileWatcher percebe o novo arquivo e aguarda estabilidade antes de enfileirar."),
        ("3", "Validar caminho", "O agente rejeita symlink/reparse point e confirma que o caminho canônico permanece dentro da pasta monitorada."),
        ("4", "Fragmentar", "FileProcessor calcula hash, divide em chunks e grava estado local no SQLite."),
        ("5", "Autenticar", "NetworkManager abre conexão UDT, responde ao challenge com HMAC e recebe READY."),
        ("6", "Enviar chunk", "O chunk viaja no SecurePacket, com metadados e payload autenticados por AES-GCM."),
        ("7", "Persistir no servidor", "FileReceiver valida identidade, hash, nonce e quota antes de escrever no namespace do cliente."),
        ("8", "Confirmar entrega", "Servidor envia ACK autenticado; agente registra sucesso e remove pendências quando completo."),
    ]

    x_left = MARGIN + 40
    y = 290
    for num, title, body in steps:
        draw.ellipse((x_left, y, x_left + 54, y + 54), fill=COLORS["server_line"])
        tw = text_width(draw, num, F["h3"])
        draw.text((x_left + 27 - tw / 2, y + 12), num, font=F["h3"], fill=COLORS["white"])
        draw.text((x_left + 82, y - 4), title, font=F["h2"], fill=COLORS["ink"])
        draw_wrapped(draw, (x_left + 82, y + 35), body, F["small"], COLORS["muted"], PAGE_W - x_left - 2 * MARGIN - 82, 5)
        if num != "8":
            draw.line((x_left + 27, y + 58, x_left + 27, y + 118), fill=COLORS["line"], width=3)
        y += 135
    return img


def storage_page() -> Image.Image:
    img, draw = page(6, "Organização dos dados")
    box(
        draw,
        (MARGIN, 155, PAGE_W - MARGIN, 305),
        "Namespace por cliente",
        "Arquivos e registros são separados pelo client_id autenticado. Isso evita colisão entre clientes atrás do mesmo NAT ou processos diferentes no mesmo host.",
        COLORS["server"],
        COLORS["server_line"],
        F["h2"],
        F["body"],
    )

    base_x = 155
    base_y = 430
    draw.text((base_x, base_y), "server-reconstructed/", font=F["mono"], fill=COLORS["ink"])
    tree = [
        (1, "id_e2e-agent/"),
        (2, "agent-watch/"),
        (3, "e2e_payload.bin"),
        (1, "id_outro-cliente/"),
        (2, "agent-watch/"),
        (3, "relatorio.csv"),
    ]
    y = base_y + 45
    for level, name in tree:
        x = base_x + level * 54
        draw.line((x - 28, y + 12, x - 8, y + 12), fill=COLORS["line"], width=2)
        draw.text((x, y), name, font=F["mono"], fill=COLORS["ink"] if level < 3 else COLORS["server_line"])
        y += 46

    arrow(draw, (760, 555), (980, 555), COLORS["server_line"], 4, "consulta")
    box(
        draw,
        (850, 660, 1130, 830),
        "SQLite servidor",
        "Estado de chunks, progresso, hash final, quotas e idempotência.",
        COLORS["core"],
        COLORS["core_line"],
    )
    arrow(draw, (980, 660), (980, 585), COLORS["core_line"], 4)

    draw.text((MARGIN, 965), "Efeito prático", font=F["h1"], fill=COLORS["ink"])
    bullet_list(
        draw,
        MARGIN,
        1035,
        [
            "Dois agentes com o mesmo IP não competem pelo mesmo arquivo lógico.",
            "Falhas e reenvios podem retomar pelo estado persistido, sem misturar chunks de outro cliente.",
            "Rotinas de backup, auditoria e limpeza conseguem operar por cliente.",
        ],
        PAGE_W - 2 * MARGIN,
    )
    return img


def validation_page() -> Image.Image:
    img, draw = page(7, "Validação end-to-end")
    draw_wrapped(
        draw,
        (MARGIN, 140),
        "A validação executada no workspace testou o caminho real de envio e um cenário negativo de quota. Os resultados abaixo resumem o comportamento observado depois das melhorias.",
        F["body"],
        COLORS["ink"],
        PAGE_W - 2 * MARGIN,
        8,
    )

    box(
        draw,
        (MARGIN, 290, PAGE_W - MARGIN, 510),
        "Envio positivo",
        "Arquivo de 24.576 bytes transferido e reconstruído em server-reconstructed/id_e2e-agent/agent-watch/e2e_payload.bin. SHA-256 confirmado: 95B6FD038BDCBB4B659313E997E5950E2917C74CEFC2579FE38F7FDE013A3239.",
        COLORS["agent"],
        COLORS["agent_line"],
        F["h2"],
        F["body"],
    )
    box(
        draw,
        (MARGIN, 560, PAGE_W - MARGIN, 780),
        "Rejeição por limite",
        "Com server.max_file_size_mb=1, um arquivo de 2.097.152 bytes foi rejeitado pelo servidor e nenhum arquivo reconstruído foi produzido.",
        COLORS["warn"],
        COLORS["warn_line"],
        F["h2"],
        F["body"],
    )
    box(
        draw,
        (MARGIN, 830, PAGE_W - MARGIN, 1050),
        "Testes de unidade e build",
        "Server: 25/25 testes. Agent: 27/27 testes. Builds Debug de Agent e Server executados com sucesso.",
        COLORS["server"],
        COLORS["server_line"],
        F["h2"],
        F["body"],
    )

    draw.text((MARGIN, 1185), "Leitura operacional", font=F["h1"], fill=COLORS["ink"])
    bullet_list(
        draw,
        MARGIN,
        1255,
        [
            "O fluxo principal entrega e reconstrói o arquivo esperado.",
            "A quota atua antes de materializar arquivos grandes indevidos.",
            "Os testes cobrem os principais controles P1/P2 adicionados nesta rodada.",
        ],
        PAGE_W - 2 * MARGIN,
    )
    return img


def main() -> None:
    pages = [
        cover_page(),
        architecture_page(),
        sequence_page(),
        security_page(),
        use_case_page(),
        storage_page(),
        validation_page(),
    ]
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    pages[0].save(OUTPUT, "PDF", save_all=True, append_images=pages[1:], resolution=150.0)
    print(f"PDF gerado: {OUTPUT}")
    print(f"Paginas: {len(pages)}")
    print(f"Tamanho: {OUTPUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
