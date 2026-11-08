# Reads the ArkHelper IPC shared-memory sections and prints the current payload.
#
# This is a development/diagnostic tool only: the product uses the C++ contract in
# include/arkhelp/ipc.h. The structs below mirror that header (v8) and must be kept in sync
# when the contract version is bumped; the static asserts in the C# helper catch layout drift.
#
# Usage:
#   pwsh ./read_ipc.ps1                 # one sample
#   pwsh ./read_ipc.ps1 -Loop -Count 10 # sample every second, 10 times
[CmdletBinding()]
param(
    [switch]$Loop,
    [int]$Count = 1
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class ArkHelperIpcReader
{
    const uint BattleMagic = 0x42484841u;
    const uint CommandMagic = 0x43484841u;
    const int MaxSpawns = 48;
    const int MaxEnemies = 48;
    const int MaxDamageRows = 64;

    static ArkHelperIpcReader()
    {
        if (Marshal.SizeOf<SpawnEntry>() != 188 || Marshal.SizeOf<EnemyEntry>() != 248 ||
            Marshal.SizeOf<DamageEntry>() != 296 || Marshal.SizeOf<UnitEntry>() != 176 ||
            Marshal.SizeOf<PathEntry>() != 1184 ||
            Marshal.SizeOf<BattleSnapshot>() != 146696 || Marshal.SizeOf<CommandState>() != 560)
        { throw new InvalidOperationException("IPC v8 layout mismatch"); }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Vec2 { public int x; public int y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct Float2 { public float x; public float y; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct SpawnEntry
    {
        public uint planned_frame;
        public uint count;
        public uint route_index;
        public uint flags;
        public Vec2 screen;
        public float remaining_seconds;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 96)] public byte[] enemy_key;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] enemy_name;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct EnemyEntry
    {
        public uint flags;
        public uint reserved;
        public Vec2 screen;
        public long hp;
        public long max_hp;
        public int atk, def, magic_resistance, move_speed, attack_speed, base_attack_time;
        public int weight, block_cnt, life_point_reduce;
        public uint damage_type_mask, stats_mask;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 96)] public byte[] enemy_key;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] enemy_name;
        public ulong tracking_id;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct DamageEntry
    {
        public uint flags;
        public uint damage_type;
        public Vec2 screen;
        public long total_damage;
        public float dps;
        public float share;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 96)] public byte[] actor_id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] actor_name;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12)] public double[] metrics;
        public uint metric_mask, reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct UnitEntry
    {
        public float x, y, hp, max_hp;
        public uint flags;
        public ulong tracking_id;
        public float ground_x, ground_y, range_radius;
        public uint range_point_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)] public Float2[] range_points;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PathPoint { public float x, y; public uint flags; }

    [StructLayout(LayoutKind.Sequential)]
    public struct PathEntry
    {
        public ulong tracking_id, sampled_ms;
        public uint battle_serial, flags, point_count, route_revision;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 96)] public PathPoint[] points;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct BattleSnapshot
    {
        public uint magic;
        public uint version;
        public uint size;
        public uint sequence;
        public ulong published_ms;
        public uint battle_serial;
        public uint in_battle;
        public int battle_state;
        public uint flags;
        public uint frame;
        public uint spawn_count;
        public uint enemy_count;
        public uint damage_count;
        public uint unit_count;
        public uint path_count;
        public double time_seconds;
        public ulong unit_sample_ms;
        public double unit_sample_battle_time;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)] public byte[] level_id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MaxSpawns)] public SpawnEntry[] spawns;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MaxEnemies)] public EnemyEntry[] enemies;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MaxDamageRows)] public DamageEntry[] damage;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public UnitEntry[] units;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public PathEntry[] paths;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 12)] public double[] team_metrics;
        public uint team_metric_mask, stats_flags;
        public uint operator_count, operator_reserved;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public DamageEntry[] operators;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public ulong[] operator_ids;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CommandState
    {
        public uint magic;
        public uint version;
        public uint size;
        public uint sequence;
        public uint feature_mask;
        public uint card_index;
        public uint revision;
        public float ui_scale;
        public uint flags;
        public uint path_request_battle_serial, path_request_count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public ulong[] path_request_ids;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr OpenFileMappingW(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr mapping, uint access, uint offsetHigh, uint offsetLow, UIntPtr bytes);
    [DllImport("kernel32.dll")]
    static extern bool UnmapViewOfFile(IntPtr address);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);

    static string Text(byte[] bytes)
    {
        if (bytes == null) { return string.Empty; }
        int length = Array.IndexOf(bytes, (byte)0);
        if (length < 0) { length = bytes.Length; }
        return Encoding.UTF8.GetString(bytes, 0, length);
    }

    static bool Open(string name, out IntPtr mapping, out IntPtr view)
    {
        mapping = OpenFileMappingW(0x0004, false, name);
        view = IntPtr.Zero;
        if (mapping == IntPtr.Zero) { return false; }
        view = MapViewOfFile(mapping, 0x0004, 0, 0, UIntPtr.Zero);
        if (view == IntPtr.Zero) { CloseHandle(mapping); return false; }
        return true;
    }

    static bool Read<T>(IntPtr view, uint magic, out T value) where T : struct
    {
        value = default(T);
        if ((uint)Marshal.ReadInt32(view, 0) != magic || Marshal.ReadInt32(view, 4) != 8 ||
            Marshal.ReadInt32(view, 8) != Marshal.SizeOf<T>()) { return false; }
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            int sequence = Marshal.ReadInt32(view, 12);
            if ((sequence & 1) != 0) { continue; }
            Thread.MemoryBarrier();
            T copy = Marshal.PtrToStructure<T>(view);
            Thread.MemoryBarrier();
            if (sequence == Marshal.ReadInt32(view, 12)) { value = copy; return true; }
        }
        return false;
    }

    public static string Dump()
    {
        StringBuilder text = new StringBuilder();
        IntPtr mapping;
        IntPtr view;

        if (!Open(@"Local\ArkHelp.Command.v1", out mapping, out view))
        {
            text.AppendLine("command: <not mapped>");
        }
        else
        {
            try
            {
                CommandState command;
                if (!Read(view, CommandMagic, out command))
                {
                    text.AppendLine("command: incompatible or busy mapping");
                }
                else
                {
                    text.AppendLine(string.Format(
                        "command: magic=0x{0:X8} version={1} size={2} sequence={3} features=0x{4:X2} card={5} revision={6} ui_scale={7} flags=0x{8:X2}",
                        command.magic, command.version, command.size, command.sequence, command.feature_mask,
                        command.card_index, command.revision, command.ui_scale, command.flags));
                    text.AppendLine(string.Format("path requests: serial={0} count={1} instances={2}",
                        command.path_request_battle_serial, command.path_request_count,
                        string.Join(",", command.path_request_ids ?? new ulong[0])));
                }
            }
            finally
            {
                UnmapViewOfFile(view);
                CloseHandle(mapping);
            }
        }

        if (!Open(@"Local\ArkHelp.Battle.v1", out mapping, out view))
        {
            text.AppendLine("battle: <not mapped>");
            return text.ToString();
        }
        try
        {
            BattleSnapshot snapshot;
            if (!Read(view, BattleMagic, out snapshot))
            {
                text.AppendLine("battle: incompatible or busy mapping");
                return text.ToString();
            }

            int spawnCount = Math.Min((int)snapshot.spawn_count, MaxSpawns);
            int damageCount = Math.Min((int)snapshot.damage_count, MaxDamageRows);
            text.AppendLine(string.Format(
                "battle: magic=0x{0:X8} version={1} size={2} sequence={3} published_ms={4} serial={5} in_battle={6} state={7} flags=0x{8:X2} frame={9} time={10:F3}s level={11}",
                snapshot.magic, snapshot.version, snapshot.size, snapshot.sequence, snapshot.published_ms,
                snapshot.battle_serial, snapshot.in_battle, snapshot.battle_state, snapshot.flags,
                snapshot.frame, snapshot.time_seconds, Text(snapshot.level_id)));
            text.AppendLine(string.Format("counts: spawn={0} enemy={1} damage={2}", spawnCount,
                Math.Min((int)snapshot.enemy_count, MaxEnemies), damageCount));
            text.AppendLine(string.Format("unit sample: ms={0} battle_time={1:F3} count={2}",
                snapshot.unit_sample_ms, snapshot.unit_sample_battle_time, snapshot.unit_count));
            text.AppendLine(string.Format("paths: count={0}", snapshot.path_count));
            for (int index = 0; index < Math.Min((int)snapshot.path_count, 64); ++index)
            {
                PathEntry path = snapshot.paths[index];
                text.AppendLine(string.Format(
                    "path[{0}]: instance={1} serial={2} sampled_ms={3} flags=0x{4:X2} points={5} revision={6}",
                    index, path.tracking_id, path.battle_serial, path.sampled_ms,
                    path.flags, path.point_count, path.route_revision));
            }
            text.AppendLine(string.Format("team: available=0x{0:X3} flags=0x{1:X2} metrics={2}",
                snapshot.team_metric_mask, snapshot.stats_flags, string.Join(",", snapshot.team_metrics)));
            for (int index = 0; index < Math.Min((int)snapshot.unit_count, 64); ++index)
            {
                UnitEntry unit = snapshot.units[index];
                text.AppendLine(string.Format(
                    "unit[{0}]: instance={1} flags=0x{2:X2} hp={3}/{4} anchor=({5:F1},{6:F1}) ground=({7:F1},{8:F1}) range_radius={9:F3} points={10}",
                    index, unit.tracking_id, unit.flags, unit.hp, unit.max_hp, unit.x, unit.y,
                    unit.ground_x, unit.ground_y, unit.range_radius, unit.range_point_count));
            }
            for (int index = 0; index < Math.Min((int)snapshot.enemy_count, MaxEnemies); ++index)
            {
                EnemyEntry enemy = snapshot.enemies[index];
                text.AppendLine(string.Format(
                    "enemy[{0}]: instance={1} flags=0x{2:X2} available=0x{3:X3} hp={4}/{5} atk={6} def={7} MR={8:F2} key={9} name={10}",
                    index, enemy.tracking_id, enemy.flags, enemy.stats_mask, enemy.hp, enemy.max_hp,
                    enemy.atk, enemy.def, enemy.magic_resistance / 1000.0,
                    Text(enemy.enemy_key), Text(enemy.enemy_name)));
            }

            for (int index = 0; index < spawnCount; ++index)
            {
                SpawnEntry spawn = snapshot.spawns[index];
                text.AppendLine(string.Format(
                    "spawn[{0}]: frame={1} count={2} route={3} flags=0x{4:X2} screen=({5},{6}) remaining={7:F2}s key={8} name={9}",
                    index, spawn.planned_frame, spawn.count, spawn.route_index, spawn.flags,
                    spawn.screen.x, spawn.screen.y, spawn.remaining_seconds,
                    Text(spawn.enemy_key), Text(spawn.enemy_name)));
            }
            text.AppendLine(string.Format("operators: count={0}", snapshot.operator_count));
            for (int index = 0; index < Math.Min((int)snapshot.operator_count, 64); ++index)
            {
                DamageEntry actor = snapshot.operators[index];
                text.AppendLine(string.Format("operator[{0}]: instance={1} actor={2} anchor=({3},{4}) damage={5} metrics={6}",
                    index, snapshot.operator_ids[index], Text(actor.actor_id), actor.screen.x, actor.screen.y,
                    actor.total_damage, string.Join(",", actor.metrics)));
            }
            for (int index = 0; index < damageCount; ++index)
            {
                DamageEntry damage = snapshot.damage[index];
                text.AppendLine(string.Format(
                    "damage[{0}]: flags=0x{1:X2} type={2} screen=({3},{4}) total={5} dps={6:F1} share={7:F3} actor={8} name={9}",
                    index, damage.flags, damage.damage_type, damage.screen.x, damage.screen.y,
                    damage.total_damage, damage.dps, damage.share,
                    Text(damage.actor_id), Text(damage.actor_name)));
                text.AppendLine(string.Format("  supported=0x{0:X3} metrics={1}", damage.metric_mask,
                    string.Join(",", damage.metrics)));
            }
        }
        finally
        {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
        }
        return text.ToString();
    }
}
"@

for ($sample = 0; $sample -lt $Count; $sample++) {
    Write-Output "=== sample $(($sample + 1)) $(Get-Date -Format 'HH:mm:ss.fff') ==="
    Write-Output ([ArkHelperIpcReader]::Dump())
    if ($Loop -and $sample + 1 -lt $Count) { Start-Sleep -Seconds 1 }
}
