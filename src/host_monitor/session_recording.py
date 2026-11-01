"""Durable, per-capture sound pressure observations and host-side marker times."""
import json
import math
import sqlite3
import threading
from contextlib import contextmanager

from session_export import align_markers

CENTERS = [31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250]


def segment_id(session, capture, config):
    fields = [str(v) for v in (session, capture, config)]
    if not all(s.isascii() and s.isdecimal() and 0 < len(s) <= 20 and int(s) > 0 for s in fields):
        raise ValueError('需要有效的会话、捕获段和配置编号')
    return '/'.join(fields)


class SessionRecording:
    def __init__(self, directory):
        self.directory = directory
        self.lock = threading.Lock()
        self.db = None
        self.error = ''
        self.unwritten = set()

    @contextmanager
    def transaction(self, identity=None):
        with self.lock:
            try:
                if self.db is None:
                    if not self.directory:
                        raise OSError('找不到可写的记录目录')
                    self.directory.mkdir(parents=True, exist_ok=True)
                    db = sqlite3.connect(self.directory/'observations.sqlite3',check_same_thread=False)
                    try:
                        db.executescript("""
                        PRAGMA journal_mode=WAL;
                        CREATE TABLE IF NOT EXISTS segments (
                            id TEXT PRIMARY KEY, summary TEXT, warning TEXT NOT NULL DEFAULT ''
                        );
                        CREATE TABLE IF NOT EXISTS points (
                            id INTEGER PRIMARY KEY, segment TEXT NOT NULL, time_ms INTEGER NOT NULL,
                            token TEXT, native INTEGER NOT NULL, levels TEXT NOT NULL
                        );
                        CREATE INDEX IF NOT EXISTS points_segment ON points(segment,id);
                        CREATE UNIQUE INDEX IF NOT EXISTS native_token ON points(segment,token)
                            WHERE native=1;
                        CREATE TABLE IF NOT EXISTS markers (
                            segment TEXT NOT NULL, request TEXT NOT NULL, marker TEXT,
                            strength INTEGER NOT NULL, note TEXT NOT NULL, time_ms INTEGER NOT NULL,
                            PRIMARY KEY(segment,request)
                        );
                        """)
                    except Exception:
                        db.close()
                        raise
                    self.db = db
                with self.db:
                    for missed in self.unwritten:
                        self.db.execute('INSERT OR IGNORE INTO segments(id) VALUES (?)',(missed,))
                        self.db.execute("UPDATE segments SET warning='本机写盘曾失败，部分观测或标记时间可能缺失。' WHERE id=?",(missed,))
                    yield self.db
                self.unwritten.clear()
                self.error = ''
            except (sqlite3.Error, OSError) as error:
                if identity:
                    self.unwritten.add(identity)
                self.error = '整场记录文件读写失败：' + str(error)
                raise OSError(self.error) from error

    def point(self, frame, time_ms, *, serial, joined=False):
        identity, token, bands, values = frame
        indexed = {}
        for band,value in zip(bands,values):
            center = math.sqrt(band['low_hz']*band['high_hz'])
            nominal = min(CENTERS,key=lambda hz:abs(hz-center))
            if nominal in indexed or not math.isclose(nominal,center,rel_tol=.02):
                self.warn(identity,'板端频带与十个标称中心不一致，部分读数未入表。')
                return
            indexed[nominal] = value
        ordered = [indexed[c] for c in CENTERS]
        with self.transaction(identity) as db:
            db.execute('INSERT OR IGNORE INTO segments(id) VALUES (?)',(identity,))
            if joined:
                self._warning(db,identity,'上位机接入时板卡已在采集，仅保存本机在线期间的观测。')
            # UART numbering starts over after a reconnect. Only firmware frame identifiers
            # are globally meaningful within a segment; each successful UART read is new.
            db.execute('INSERT OR IGNORE INTO points(segment,time_ms,token,native,levels) VALUES (?,?,?,?,?)',
                       (identity,time_ms,token,not serial,json.dumps(ordered)))

    def marker_request(self, identity, request, strength, note, time_ms):
        with self.transaction(identity) as db:
            db.execute('INSERT OR IGNORE INTO segments(id) VALUES (?)',(identity,))
            db.execute('INSERT OR IGNORE INTO markers(segment,request,strength,note,time_ms) VALUES (?,?,?,?,?)',
                       (identity,request,strength,note,time_ms))

    def marker_reply(self, identity, request, marker_id):
        with self.transaction(identity) as db:
            db.execute('UPDATE markers SET marker=? WHERE segment=? AND request=?',(str(marker_id),identity,request))

    def marker_rejected(self, identity, request):
        with self.transaction(identity) as db:
            db.execute('DELETE FROM markers WHERE segment=? AND request=? AND marker IS NULL',(identity,request))

    def summary(self, report):
        identity = segment_id(report.get('session_id'),report.get('capture_id'),report.get('config_id'))
        with self.transaction(identity) as db:
            # An imported / historical SD report cannot create a fictitious local recording.
            db.execute('UPDATE segments SET summary=? WHERE id=?',(json.dumps(report,ensure_ascii=False),identity))

    def warn(self, identity, message='采集中曾未收到实时频谱，图表仅包含成功收到的观测。'):
        with self.transaction(identity) as db:
            db.execute('INSERT OR IGNORE INTO segments(id) VALUES (?)',(identity,))
            self._warning(db,identity,message)

    @staticmethod
    def _warning(db, identity, message):
        db.execute("UPDATE segments SET warning=trim(warning||' '||?) WHERE id=? AND instr(warning,?)=0",
                   (message,identity,message))

    def detail(self, identity=None, *, metadata=False):
        with self.transaction() as db:
            if identity is None:
                row = db.execute('SELECT segment FROM points ORDER BY id DESC LIMIT 1').fetchone()
                identity = row[0] if row else None
            row = db.execute('SELECT warning,summary IS NOT NULL FROM segments WHERE id=?',(identity,)).fetchone()
            if not row:
                raise KeyError('没有此捕获段的本机逐点记录')
            session,capture,config = identity.split('/')
            count = db.execute('SELECT count(*) FROM points WHERE segment=?',(identity,)).fetchone()[0]
            data = dict(session=session,capture=capture,config=config,count=count,warning=row[0],summary=bool(row[1]))
            if metadata:
                return data
            raw = db.execute('SELECT summary FROM segments WHERE id=?',(identity,)).fetchone()[0]
            summary = json.loads(raw) if raw else None
            points = [dict(time_ms=t,token=k,values=json.loads(v)) for t,k,v in db.execute(
                'SELECT time_ms,token,levels FROM points WHERE segment=? ORDER BY time_ms,id',(identity,))]
            requests = list(db.execute('SELECT marker,strength,note,time_ms FROM markers WHERE segment=? ORDER BY time_ms,rowid',(identity,)))
        by_id = {str(mid):t for mid,_,_,t in requests if mid is not None}
        markers = []
        synced = set()
        for m in (summary or {}).get('markers', []):
            mid = str(m['marker_id'])
            synced.add(mid)
            markers.append(dict(marker_id=mid,strength=m.get('strength'),note=m.get('note',''),
                                time_ms=by_id.get(mid),start_raw=str(m['start_raw']) if m.get('start_raw') is not None else None,
                                status='synced'))
        for mid,strength,note,t in requests:
            if mid is None or mid not in synced:
                markers.append(dict(marker_id=mid,strength=strength,note=note,time_ms=t,start_raw=None,
                                    status='queued' if mid is not None else 'unconfirmed'))
        return dict(data,summary=summary,centers=CENTERS,points=points,markers=align_markers(points,markers))

    def close(self):
        with self.lock:
            if self.db is not None:
                self.db.close()
                self.db = None
