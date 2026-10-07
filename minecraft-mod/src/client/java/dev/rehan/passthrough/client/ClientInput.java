package dev.rehan.passthrough.client;

import com.google.gson.JsonObject;
import dev.rehan.passthrough.Passthrough;
import dev.rehan.passthrough.client.mixin.KeyMappingAccessor;
import dev.rehan.passthrough.client.mixin.MouseHandlerAccessor;
import com.mojang.blaze3d.platform.Window;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.player.Inventory;
import org.lwjgl.sdl.SDLVideo;

/** Host input, applied on the client thread: the host window has the focus, so Minecraft never sees these itself. */
final class ClientInput {
	private ClientInput() {
	}

	static void handle(final Minecraft minecraft, final JsonObject m) {
		LocalPlayer player = minecraft.player;
		switch (m.get("t").getAsString()) {
			case "key" -> {
				String k = m.get("k").getAsString();
				boolean down = !m.has("down") || m.get("down").getAsBoolean();
				if (k.equals("escape")) {
					if (down && minecraft.gui.screen() != null) {
						minecraft.gui.screen().onClose();
					}

					return;
				}

				KeyMapping key = switch (k) {
					case "use" -> minecraft.options.keyUse;
					case "attack" -> minecraft.options.keyAttack;
					case "pick" -> minecraft.options.keyPickItem;
					case "inventory" -> minecraft.options.keyInventory;
					case "drop" -> minecraft.options.keyDrop;
					case "swap" -> minecraft.options.keySwapOffhand;
					default -> null;
				};
				if (k.equals("attack") && down && player != null
					&& BuiltInRegistries.ITEM.getKey(player.getMainHandItem().getItem()).getPath().endsWith("_sword")) {
					// a sword swing: the host hits what's in front of Steve in its own world
					Passthrough.events.accept("{\"t\":\"melee\"}");
				}

				if (key != null) {
					if (down && !key.isDown()) {
						KeyMappingAccessor access = (KeyMappingAccessor)key;
						access.passthrough$setClickCount(access.passthrough$getClickCount() + 1);
					}

					key.setDown(down);
				}
			}
			case "cursor" -> {
				// the host's virtual cursor over Minecraft's open screen (x, y: 0..1 of the picture)
				Window window = minecraft.getWindow();
				double sx = Math.clamp(m.get("x").getAsDouble(), 0.0, 1.0) * window.getScreenWidth();
				double sy = Math.clamp(m.get("y").getAsDouble(), 0.0, 1.0) * window.getScreenHeight();
				MouseHandlerAccessor mouse = (MouseHandlerAccessor) minecraft.mouseHandler;
				mouse.passthrough$setXpos(sx);
				mouse.passthrough$setYpos(sy);
				Screen screen = minecraft.gui.screen();
				if (screen != null) {
					screen.mouseMoved(minecraft.mouseHandler.getScaledXPos(window), minecraft.mouseHandler.getScaledYPos(window));
				}
			}
			case "click" -> {
				// a mouse button on Minecraft's open screen, at the virtual cursor (Minecraft's own click handling:
				// picking up, placing, dragging, double clicks)
				if (minecraft.gui.screen() != null) {
					int button = Math.clamp(m.get("b").getAsInt(), 0, 2);
					boolean down = !m.has("down") || m.get("down").getAsBoolean();
					minecraft.mouseHandler.onButton(minecraft.getWindow().handle(), new MouseButtonInfo(button, 0), down ? 1 : 0);
				}
			}
			case "wheel" -> {
				Screen screen = minecraft.gui.screen();
				if (screen != null) {
					Window window = minecraft.getWindow();
					screen.mouseScrolled(minecraft.mouseHandler.getScaledXPos(window), minecraft.mouseHandler.getScaledYPos(window), 0.0,
						m.get("d").getAsDouble());
				}
			}
			case "slot" -> {
				if (player != null) {
					player.getInventory().setSelectedSlot(Math.clamp(m.get("n").getAsInt(), 0, Inventory.getSelectionSize() - 1));
				}
			}
			case "scroll" -> {
				if (player != null) {
					Inventory inventory = player.getInventory();
					int size = Inventory.getSelectionSize();
					inventory.setSelectedSlot(Math.floorMod(inventory.getSelectedSlot() - m.get("d").getAsInt(), size));
				}
			}
			case "hud" -> {
				if (minecraft.gui.hud.isHidden() != m.get("hidden").getAsBoolean()) {
					minecraft.gui.hud.toggle();
				}
			}
			case "fps" -> {
				// Minecraft's frame cap ({"t":"fps","n":10-260}): faster than the host only competes with it for the GPU
				int n = Math.max(10, Math.min(260, m.get("n").getAsInt()));
				minecraft.options.framerateLimit().set(n);
				Passthrough.LOG.info("frame rate limit {}", n);
			}
			case "view" -> {
				// match the host's picture exactly: un-minimize/un-maximize first (resizing a maximized window is ignored)
				int w = m.get("w").getAsInt(), h = m.get("h").getAsInt();
				long handle = minecraft.getWindow().handle();
				SDLVideo.SDL_RestoreWindow(handle);
				minecraft.getWindow().setWindowed(w, h);
				SDLVideo.SDL_SetWindowSize(handle, w, h);
				SDLVideo.SDL_SyncWindow(handle);
			}
			default -> {
			}
		}
	}
}
