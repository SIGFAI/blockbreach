package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.WorldBridge;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The host's floors and walls are barrier blocks, there for mobs, items and dropped blocks to stand on. Steve's arrows
 * and fireworks fly through them: the host traces those through its own world (the same way its bullets go) and tells
 * Minecraft where they really hit ({"t":"projhit"}), so a barrier column never stops a shot a bullet would pass. Other
 * projectiles aren't traced by the host and still collide with the barriers (WorldBridge.hostTraced).
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
abstract class BarrierProjectileMixin {
	@Inject(method = "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/phys/shapes/CollisionContext;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$projectilesIgnoreBarriers(final BlockGetter level, final BlockPos pos, final CollisionContext context,
		final CallbackInfoReturnable<VoxelShape> cir) {
		if (context instanceof EntityCollisionContext entity && entity.getEntity() instanceof Projectile projectile
			&& WorldBridge.hostTraced(projectile) && ((BlockBehaviour.BlockStateBase) (Object) this).is(Blocks.BARRIER)) {
			cir.setReturnValue(Shapes.empty());
		}
	}
}
