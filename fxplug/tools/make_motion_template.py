#!/usr/bin/env python3
"""Write the Final Cut Pro effect template (.moef) for the NTSCRT FxPlug filter.

Final Cut Pro only lists FxPlug effects through Motion templates: a "Final Cut
Effect" document whose Effect Source layer carries the filter and publishes its
parameters. This generates a minimal one that applies the filter (pluginUUID)
and publishes the plug-in's top-level parameter group (ID 1, which contains
every control), so all parameters appear in FCP's inspector.

If a template saved by Motion itself exists at fxplug/motion/NTSCRT/NTSCRT.moef
it is copied instead (see fxplug/README.md, "Motion template"): Motion's own
output is the reference format and always wins over this generator.

  make_motion_template.py --uuid UUID --name NTSCRT --out DIR/NTSCRT/NTSCRT.moef
"""
import argparse
import os
import shutil
from xml.sax.saxutils import quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
OVERRIDE = os.path.normpath(os.path.join(HERE, "..", "motion", "NTSCRT", "NTSCRT.moef"))

# Motion's built-in object factories (class identifiers used by every .moef).
FACTORIES = [
    (1, "46c844a813d311d8a438000a95af9f7e", "Channel"),
    (2, "65cb4dc9d4504fa281921f5f751fba06", "Widget"),
    (3, "66fc0d6af6a911d6a7a7000393670732", "Image"),
    (4, "7d468273c013498e9806a0d7bc32fddf", "Project"),
    (5, "dbca752470fd11d7980100039389b702", "Channel"),
    (6, "deca4859b16011d7a12d0003936f6f92", "ProPlugin Filter"),
]

# 10 s at 30 fps in Motion's rational time ("value timescale flags epoch").
DURATION = "300 30 1 0"
ZERO = "0 1 1 0"


def build(uuid, name):
    fac = "\n".join(
        f'<factory id="{i}" uuid="{u}">\n\t<description>{d}</description>\n'
        f"\t<manufacturer>Apple</manufacturer>\n\t<version>1</version>\n</factory>\n"
        for i, u, d in FACTORIES)
    n = quoteattr(name)
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ozxmlscene>
<ozml version="5.13">

<displayversion>5.6.4</displayversion>

{fac}
<description>Analog TV / VHS / CRT look</description>

<scene>
	<sceneSettings>
		<width>1920</width>
		<height>1080</height>
		<duration>300</duration>
		<frameRate>30</frameRate>
		<NTSC>0</NTSC>
		<pixelAspectRatio>1</pixelAspectRatio>
		<fieldRenderingMode>0</fieldRenderingMode>
		<backgroundColor>0 0 0 0</backgroundColor>
		<motionBlurSamples>8</motionBlurSamples>
		<motionBlurDuration>1</motionBlurDuration>
		<animateFlag>0</animateFlag>
	</sceneSettings>
	<publishSettings>
		<version>2</version>
		<target object="10036" channel="./1" name={n}/>
	</publishSettings>
	<currentFrame>{ZERO}</currentFrame>
	<currentObject>10016</currentObject>
	<activeLayer>10013</activeLayer>
	<timeRange offset="{ZERO}" duration="{DURATION}"/>
	<playRange offset="{ZERO}" duration="{DURATION}"/>
	<flags>1</flags>
	<audioTracks>0</audioTracks>
	<timemarkerset/>
	<guideset/>
	<curvesets selected="1"/>
	<layer name="Group" id="10013">
		<scenenode name="Effect Source" id="10016" factoryID="3" version="5">
			<validTracks>1</validTracks>
			<aspectRatio>1</aspectRatio>
			<flags>0</flags>
			<timing in="{ZERO}" out="{DURATION}" offset="{ZERO}"/>
			<foldFlags>0</foldFlags>
			<baseFlags>524304</baseFlags>
			<parameter name="Properties" id="1" flags="8589938704">
				<parameter name="Media" id="324" flags="8589938704">
					<parameter name="Source Media" id="300" flags="81621221392" default="10014" value="10014"/>
				</parameter>
			</parameter>
			<parameter name="Object" id="2" flags="8589938704">
				<parameter name="Drop Zone" id="311" flags="8589934738" default="0" value="1"/>
				<parameter name="Type" id="321" flags="8590000146" default="0" value="3"/>
			</parameter>
			<filter name={n} id="10036" factoryID="6" pluginUUID="{uuid}" pluginVersion="1" pluginName={n} pluginDynamicParams="0">
				<timing in="{ZERO}" out="{DURATION}" offset="{ZERO}"/>
				<baseFlags>8589934609</baseFlags>
				<parameter name={n} id="1" flags="8589938704"/>
				<parameter name="Mix" id="10001" flags="12884901888" default="1" value="1"/>
			</filter>
		</scenenode>
		<aspectRatio>1</aspectRatio>
		<flags>0</flags>
		<timing in="{ZERO}" out="{DURATION}" offset="{ZERO}"/>
		<foldFlags>0</foldFlags>
		<baseFlags>524304</baseFlags>
	</layer>
	<footage name="Media Layer" id="10015">
		<clip name="Effect Source" id="10014">
			<pathURL>Drop Zone.tiff</pathURL>
			<missingWidth>1920</missingWidth>
			<missingHeight>1080</missingHeight>
			<missingDuration>10</missingDuration>
			<flags>0</flags>
			<timing in="{ZERO}" out="{DURATION}" offset="{ZERO}"/>
			<foldFlags>0</foldFlags>
			<baseFlags>524304</baseFlags>
			<parameter name="Object" id="2" flags="8589938704">
				<parameter name="Missing Is Still" id="128" flags="8589934610" default="0" value="1"/>
			</parameter>
		</clip>
		<flags>0</flags>
		<timing in="{ZERO}" out="{DURATION}" offset="{ZERO}"/>
		<foldFlags>0</foldFlags>
		<baseFlags>524304</baseFlags>
	</footage>
</scene>

</ozml>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uuid", required=True)
    ap.add_argument("--name", default="NTSCRT")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    if os.path.exists(OVERRIDE):
        shutil.copyfile(OVERRIDE, a.out)
        print(f"copied Motion-authored template {OVERRIDE}")
        return
    # No Project Loop End marker on purpose: the effect is temporal (Apple: only
    # non-temporal templates should set one, else FCP caches across edits).
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(build(a.uuid, a.name))
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
